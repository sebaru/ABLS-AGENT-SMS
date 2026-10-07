/******************************************************************************************************************************/
/* ABLS-AGENT-SMS/src/sms.c                Gestion de l'agent SMS via ModemManager, OVH et Free Mobile                        */
/* Projet Abls-Habitat                   Gestion d'habitat                                                15.09.2026 12:00:00 */
/* Auteur: LEFEVRE Sebastien                                                                                                  */
/******************************************************************************************************************************/
/*
 * sms.c
 * This file is part of Abls-Habitat
 *
 * Copyright (C) 1988-2026 - Sebastien LEFEVRE
 *
 * ABLS-AGENT-SMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * ABLS-AGENT-SMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ABLS-AGENT-SMS; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

 #include <string.h>
 #include <unistd.h>
 #include <gio/gio.h>
 #include <curl/curl.h>
 #include <openssl/evp.h>

 #include "sms.h"

 #define MM_DBUS_SERVICE           "org.freedesktop.ModemManager1"
 #define MM_DBUS_ROOT_OBJECT       "/org/freedesktop/ModemManager1"
 #define MM_DBUS_OBJ_MANAGER_IFACE "org.freedesktop.DBus.ObjectManager"
 #define MM_DBUS_PROPERTIES_IFACE  "org.freedesktop.DBus.Properties"
 #define MM_DBUS_MODEM_IFACE       "org.freedesktop.ModemManager1.Modem"
 #define MM_DBUS_MESSAGING_IFACE   "org.freedesktop.ModemManager1.Modem.Messaging"
 #define MM_DBUS_SMS_IFACE         "org.freedesktop.ModemManager1.Sms"
 #define MM_SMS_STATE_RECEIVED     3
 #define SMS_DEFAULT_READ_INTERVAL 50

 struct SMS_HTTP_BUFFER
  { gchar *body;
    size_t size;
  };

 struct ABLS_AGENT *Agent = NULL;
 struct ABLS_SMS_VARS *Agent_vars = NULL;

/******************************************************************************************************************************/
/* Sms_http_write_cb: Callback CURL d'accumulation du corps de la reponse HTTP                                                */
/* Entrée: le fragment recu, sa taille et le buffer de destination                                                            */
/* Sortie: le nombre d'octets traites, 0 en cas d'erreur                                                                      */
/******************************************************************************************************************************/
static size_t Sms_http_write_cb ( void *contents, size_t size, size_t nmemb, void *userp )
 { struct SMS_HTTP_BUFFER *buffer = userp;
   size_t chunk_size = size * nmemb;
   gchar *ptr = g_try_realloc ( buffer->body, buffer->size + chunk_size + 1 );
   if (!ptr)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ALERT, "Realloc failed" );
      return(0);
    }

   buffer->body = ptr;
   memcpy ( buffer->body + buffer->size, contents, chunk_size );
   buffer->size += chunk_size;
   buffer->body[buffer->size] = 0;
   return(chunk_size);
 }
/******************************************************************************************************************************/
/* Sms_http_request: Envoie une requete HTTP et recupere la reponse au format Json                                            */
/* Entrée: l'url, le payload Json (NULL pour un GET) et la liste des entetes                                                  */
/* Sortie: la reponse Json enrichie de 'http_code', NULL si erreur                                                            */
/******************************************************************************************************************************/
static JsonNode *Sms_http_request ( gchar *url, JsonNode *json_payload, GSList *headers )
 { CURL *curl;
   CURLcode res;
   long http_code = 0;
   gchar *payload = NULL;
   struct curl_slist *curl_headers = NULL;
   struct SMS_HTTP_BUFFER buffer = { 0 };
   JsonNode *response = NULL;

   if (!url) return(NULL);
   curl = curl_easy_init();
   if (!curl)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "Request to %s: curl init failed", url );
      return(NULL);
    }

   if (json_payload)
    { payload = Json_to_string ( json_payload );
      curl_headers = curl_slist_append ( curl_headers, "Content-Type: application/json" );
      curl_easy_setopt ( curl, CURLOPT_POST, 1L );
      curl_easy_setopt ( curl, CURLOPT_POSTFIELDS, payload );
    }

   for (GSList *iter = headers; iter; iter = g_slist_next(iter))
    { curl_headers = curl_slist_append ( curl_headers, iter->data ); }

   curl_easy_setopt ( curl, CURLOPT_URL, url );
   curl_easy_setopt ( curl, CURLOPT_FOLLOWLOCATION, 1L );
   curl_easy_setopt ( curl, CURLOPT_CONNECTTIMEOUT, 10L );
   curl_easy_setopt ( curl, CURLOPT_TIMEOUT, 15L );
   curl_easy_setopt ( curl, CURLOPT_WRITEFUNCTION, Sms_http_write_cb );
   curl_easy_setopt ( curl, CURLOPT_WRITEDATA, &buffer );
   if (curl_headers) curl_easy_setopt ( curl, CURLOPT_HTTPHEADER, curl_headers );

   res = curl_easy_perform ( curl );
   curl_easy_getinfo ( curl, CURLINFO_RESPONSE_CODE, &http_code );
   if (res != CURLE_OK)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
            "Request to %s failed: %s", url, curl_easy_strerror(res) );
    }

   if (buffer.body) response = Json_get_from_string ( buffer.body );
   if (!response) response = Json_create();
   if (response) Json_add_int ( response, "http_code", http_code );

   if (buffer.body) g_free ( buffer.body );
   if (payload) g_free ( payload );
   if (curl_headers) curl_slist_free_all ( curl_headers );
   curl_easy_cleanup ( curl );
   return(response);
 }
/******************************************************************************************************************************/
/* Sms_get_modem_path: Recherche le chemin D-Bus du modem ModemManager et le met en cache                                     */
/* Entrée: l'adresse ou stocker le chemin alloue                                                                              */
/* Sortie: TRUE si un modem supportant le Messaging a ete trouve                                                              */
/******************************************************************************************************************************/
static gboolean Sms_get_modem_path ( gchar **modem_path )
 { GDBusConnection *system_bus;
   GVariant *reply;
   GVariantIter *objects;
   const gchar *object_path;
   GVariant *interfaces;
   GError *error = NULL;

   if (Agent_vars->mm_modem_path)
    { *modem_path = g_strdup ( Agent_vars->mm_modem_path );
      return(TRUE);
    }

   system_bus = g_bus_get_sync ( G_BUS_TYPE_SYSTEM, NULL, &error );
   if (!system_bus)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
            "Cannot connect to system D-Bus (%s)", error ? error->message : "unknown" );
      g_clear_error ( &error );
      return(FALSE);
    }

   reply = g_dbus_connection_call_sync ( system_bus, MM_DBUS_SERVICE, MM_DBUS_ROOT_OBJECT,
                                         MM_DBUS_OBJ_MANAGER_IFACE, "GetManagedObjects", NULL,
                                         G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
                                         10000, NULL, &error );
   if (!reply)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
            "GetManagedObjects failed (%s)", error ? error->message : "unknown" );
      g_clear_error ( &error );
      g_object_unref ( system_bus );
      return(FALSE);
    }

   g_variant_get ( reply, "(a{oa{sa{sv}}})", &objects );
   while ( g_variant_iter_next ( objects, "{&o@a{sa{sv}}}", &object_path, &interfaces ) )
    { GVariant *messaging = g_variant_lookup_value ( interfaces, MM_DBUS_MESSAGING_IFACE, NULL );
      if (messaging)
       { Agent_vars->mm_modem_path = g_strdup ( object_path );
         *modem_path = g_strdup ( object_path );
         g_variant_unref ( messaging );
         g_variant_unref ( interfaces );
         break;
       }
      g_variant_unref ( interfaces );
    }

   g_variant_iter_free ( objects );
   g_variant_unref ( reply );
   g_object_unref ( system_bus );
   if (*modem_path == NULL)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_WARNING,
            "No ModemManager modem with Messaging interface found" );
      return(FALSE);
    }
   return(TRUE);
 }
/******************************************************************************************************************************/
/* Sms_get_signal_quality: Lit la qualite du signal GSM du modem                                                              */
/* Entrée: l'adresse ou stocker la qualite en pourcentage                                                                     */
/* Sortie: TRUE si la lecture a abouti                                                                                        */
/******************************************************************************************************************************/
static gboolean Sms_get_signal_quality ( gdouble *signal_quality )
 { gchar *modem_path = NULL;
   GDBusConnection *system_bus;
   GVariant *reply;
   GVariant *value;
   GError *error = NULL;
   guint32 quality = 0;

   if (!Sms_get_modem_path ( &modem_path )) return(FALSE);
   system_bus = g_bus_get_sync ( G_BUS_TYPE_SYSTEM, NULL, &error );
   if (!system_bus)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
            "Cannot connect to system D-Bus (%s)", error ? error->message : "unknown" );
      g_clear_error ( &error );
      g_free ( modem_path );
      return(FALSE);
    }

   reply = g_dbus_connection_call_sync ( system_bus, MM_DBUS_SERVICE, modem_path,
                                         MM_DBUS_PROPERTIES_IFACE, "Get",
                                         g_variant_new ( "(ss)", MM_DBUS_MODEM_IFACE, "SignalQuality" ),
                                         G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
                                         10000, NULL, &error );
   if (!reply)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_WARNING,
            "Cannot read signal quality (%s)", error ? error->message : "unknown" );
      g_clear_error ( &error );
      g_object_unref ( system_bus );
      g_free ( modem_path );
      return(FALSE);
    }

   g_variant_get ( reply, "(v)", &value );
   if ( g_variant_is_of_type ( value, G_VARIANT_TYPE("(ub)") ) )
    { gboolean recent;
      g_variant_get ( value, "(ub)", &quality, &recent );
    }
   else if ( g_variant_is_of_type ( value, G_VARIANT_TYPE_UINT32 ) )
    { quality = g_variant_get_uint32 ( value ); }
   else
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_WARNING,
            "Unsupported SignalQuality type '%s'", g_variant_get_type_string(value) );
      g_variant_unref ( value );
      g_variant_unref ( reply );
      g_object_unref ( system_bus );
      g_free ( modem_path );
      return(FALSE);
    }

   *signal_quality = quality;
   g_variant_unref ( value );
   g_variant_unref ( reply );
   g_object_unref ( system_bus );
   g_free ( modem_path );
   return(TRUE);
 }
/******************************************************************************************************************************/
/* Sms_send_gsm: Envoie un SMS via le modem GSM local (ModemManager)                                                          */
/* Entrée: le message Json et le numero de telephone du destinataire                                                          */
/* Sortie: TRUE si le SMS a ete envoye                                                                                        */
/******************************************************************************************************************************/
static gboolean Sms_send_gsm ( JsonNode *msg, gchar *telephone )
 { GDBusConnection *system_bus;
   gchar *modem_path = NULL;
   gchar *sms_path = NULL;
   GVariantBuilder builder;
   GVariant *reply;
   gchar libelle[256];
   GError *error = NULL;

   if (!telephone)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "telephone is missing" );
      return(FALSE);
    }
   if (!Sms_get_modem_path ( &modem_path ))
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
            "No modem available, cannot send SMS to '%s'", telephone );
      return(FALSE);
    }

   gchar *dls_shortname = Json_get_string ( msg, "dls_shortname" );
   if (dls_shortname) g_snprintf ( libelle, sizeof(libelle), "%s: %s", dls_shortname, Json_get_string ( msg, "libelle" ) );
                 else g_snprintf ( libelle, sizeof(libelle), "%s", Json_get_string ( msg, "libelle" ) );

   Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_DEBUG,
         "Try to send to %s (%s)", telephone, libelle );

   system_bus = g_bus_get_sync ( G_BUS_TYPE_SYSTEM, NULL, &error );
   if (!system_bus)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
            "Cannot connect to system D-Bus (%s)", error ? error->message : "unknown" );
      g_clear_error ( &error );
      g_free ( modem_path );
      return(FALSE);
    }

   g_variant_builder_init ( &builder, G_VARIANT_TYPE("a{sv}") );
   g_variant_builder_add ( &builder, "{sv}", "number", g_variant_new_string(telephone) );
   g_variant_builder_add ( &builder, "{sv}", "text", g_variant_new_string(libelle) );

   reply = g_dbus_connection_call_sync ( system_bus, MM_DBUS_SERVICE, modem_path,
                                         MM_DBUS_MESSAGING_IFACE, "Create",
                                         g_variant_new ( "(a{sv})", &builder ),
                                         G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE,
                                         10000, NULL, &error );
   if (!reply)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
            "ModemManager Create failed (%s)", error ? error->message : "unknown" );
      g_clear_error ( &error );
      g_object_unref ( system_bus );
      g_free ( modem_path );
      return(FALSE);
    }

   const gchar *sms_path_tmp;
   g_variant_get ( reply, "(&o)", &sms_path_tmp );
   sms_path = g_strdup ( sms_path_tmp );
   g_variant_unref ( reply );

   reply = g_dbus_connection_call_sync ( system_bus, MM_DBUS_SERVICE, sms_path,
                                         MM_DBUS_SMS_IFACE, "Send", NULL, NULL,
                                         G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error );
   if (!reply)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_WARNING,
            "Envoi SMS Nok to %s (%s) -> %s", telephone, libelle, error ? error->message : "unknown" );
      g_clear_error ( &error );
      g_object_unref ( system_bus );
      g_free ( modem_path );
      g_free ( sms_path );
      return(FALSE);
    }
   g_variant_unref ( reply );

   reply = g_dbus_connection_call_sync ( system_bus, MM_DBUS_SERVICE, modem_path,
                                         MM_DBUS_MESSAGING_IFACE, "Delete",
                                         g_variant_new ( "(o)", sms_path ), NULL,
                                         G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL );
   if (reply) g_variant_unref ( reply );

   Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_NOTICE,
         "Envoi SMS Ok to %s (%s)", telephone, libelle );
   g_object_unref ( system_bus );
   g_free ( modem_path );
   g_free ( sms_path );
   return(TRUE);
 }
/******************************************************************************************************************************/
/* Sms_ovh_is_configured: Teste si les parametres de l'API SMS OVH sont tous renseignes                                       */
/* Entrée: néant                                                                                                              */
/* Sortie: TRUE si la configuration OVH est complete                                                                          */
/******************************************************************************************************************************/
static gboolean Sms_ovh_is_configured ( void )
 { gchar *ovh_service_name = Agent_config_get_string ( Agent, "ovh_service_name" );
   gchar *ovh_application_key = Agent_config_get_string ( Agent, "ovh_application_key" );
   gchar *ovh_application_secret = Agent_config_get_string ( Agent, "ovh_application_secret" );
   gchar *ovh_consumer_key = Agent_config_get_string ( Agent, "ovh_consumer_key" );
   return(ovh_service_name && *ovh_service_name &&
          ovh_application_key && *ovh_application_key &&
          ovh_application_secret && *ovh_application_secret &&
          ovh_consumer_key && *ovh_consumer_key);
 }
/******************************************************************************************************************************/
/* Sms_send_ovh: Envoie un SMS via l'API SMS d'OVH                                                                            */
/* Entrée: le message Json et le numero de telephone du destinataire                                                          */
/* Sortie: néant                                                                                                              */
/******************************************************************************************************************************/
static void Sms_send_ovh ( JsonNode *msg, gchar *telephone )
 { gchar clair[512], hash_string[48], signature[48], query[256];
   unsigned char hash_bin[EVP_MAX_MD_SIZE];
   unsigned int md_len;
   EVP_MD_CTX *mdctx;
   JsonNode *response;
   gchar *body;

   if (!Sms_ovh_is_configured())
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "OVH SMS is not configured" );
      return;
    }

   JsonNode *RootNode = Json_create();
   Json_add_bool ( RootNode, "noStopClause", TRUE );
   Json_add_string ( RootNode, "priority", "high" );
   Json_add_bool ( RootNode, "senderForResponse", TRUE );
   Json_add_int ( RootNode, "validityPeriod", 2880 );
   Json_add_string ( RootNode, "charset", "UTF-8" );

   JsonArray *receivers = Json_add_array ( RootNode, "receivers" );
   Json_array_add_element ( receivers, json_node_init_string ( json_node_alloc(), telephone ) );

   gchar libelle[256];
   g_snprintf ( libelle, sizeof(libelle), "%s: %s", Json_get_string ( msg, "dls_shortname" ), Json_get_string ( msg, "libelle" ) );
   Json_add_string ( RootNode, "message", libelle );

   g_snprintf ( query, sizeof(query), "https://eu.api.ovh.com/1.0/sms/%s/jobs", Agent_config_get_string ( Agent, "ovh_service_name" ) );
   gchar timestamp[20];
   g_snprintf ( timestamp, sizeof(timestamp), "%ld", time(NULL) );

   body = Json_to_string ( RootNode );
   g_snprintf ( clair, sizeof(clair), "%s+%s+POST+%s+%s+%s",
                Agent_config_get_string ( Agent, "ovh_application_secret" ),
                Agent_config_get_string ( Agent, "ovh_consumer_key" ),
                query, body, timestamp );
   Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_DEBUG, "Sending to OVH : %s", body );

   mdctx = EVP_MD_CTX_new();
   EVP_DigestInit_ex ( mdctx, EVP_sha1(), NULL );
   EVP_DigestUpdate ( mdctx, clair, strlen(clair) );
   EVP_DigestFinal_ex ( mdctx, hash_bin, &md_len );
   EVP_MD_CTX_free ( mdctx );

   memset ( hash_string, 0, sizeof(hash_string) );
   for (gint i = 0; i < 20; i++)
    { gchar chaine[3];
      g_snprintf ( chaine, sizeof(chaine), "%02x", hash_bin[i] );
      g_strlcat ( hash_string, chaine, sizeof(hash_string) );
    }
   g_snprintf ( signature, sizeof(signature), "$1$%s", hash_string );

   gchar header[256];
   GSList *liste = NULL;
   g_snprintf ( header, sizeof(header), "X-Ovh-Application: %s", Agent_config_get_string ( Agent, "ovh_application_key" ) );
   liste = g_slist_append ( liste, g_strdup(header) );
   g_snprintf ( header, sizeof(header), "X-Ovh-Consumer: %s", Agent_config_get_string ( Agent, "ovh_consumer_key" ) );
   liste = g_slist_append ( liste, g_strdup(header) );
   g_snprintf ( header, sizeof(header), "X-Ovh-Signature: %s", signature );
   liste = g_slist_append ( liste, g_strdup(header) );
   g_snprintf ( header, sizeof(header), "X-Ovh-Timestamp: %s", timestamp );
   liste = g_slist_append ( liste, g_strdup(header) );

   response = Sms_http_request ( query, RootNode, liste );
   gint http_code = ( response ? Json_get_int ( response, "http_code" ) : 0 );
   g_slist_free_full ( liste, g_free );
   g_free ( body );
   Json_unref ( RootNode );

   if (http_code != 200)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "Status %d", http_code ); }
   else Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_NOTICE, "'%s' sent to '%s'", libelle, telephone );
   if (response) Json_unref ( response );
 }

/******************************************************************************************************************************/
/* Sms_send_freeapi: Envoie un SMS via l'API Free Mobile de l'utilisateur                                                     */
/* Entrée: le message Json et l'utilisateur destinataire                                                                      */
/* Sortie: néant                                                                                                              */
/******************************************************************************************************************************/
static void Sms_send_freeapi ( JsonNode *msg, JsonNode *user )
 { gchar libelle_utf8[512];
   g_snprintf ( libelle_utf8, sizeof(libelle_utf8), "%s: %s", Json_get_string ( msg, "dls_shortname" ), Json_get_string ( msg, "libelle" ) );
   gchar *libelle = g_uri_escape_string ( libelle_utf8, NULL, FALSE );
   if (libelle == NULL)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "Convert error for %s. Not sending message.", libelle_utf8 );
      return;
    }

   gchar target_uri[512];
   g_snprintf ( target_uri, sizeof(target_uri), "https://smsapi.free-mobile.fr/sendmsg?user=%s&pass=%s&msg=%s",
                Json_get_string ( user, "free_sms_api_user" ), Json_get_string ( user, "free_sms_api_key" ), libelle );
   g_free ( libelle );

   JsonNode *response = Http_Get_external ( Agent, target_uri );
   gint http_code = ( response ? Json_get_int ( response, "http_code" ) : 0 );
   if (response) Json_unref ( response );
   if (http_code != 200)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "Status %d for '%s' to '%s'",
            http_code, libelle_utf8, Json_get_string ( user, "email" ) );
    }
   else Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_NOTICE, "'%s' sent to '%s'", libelle_utf8, Json_get_string ( user, "email" ) );
 }

/******************************************************************************************************************************/
/* Sms_send_to_all_authorized_recipients: Diffuse un message a tous les utilisateurs a notifier                               */
/* Entrée: le message Json                                                                                                    */
/* Sortie: néant                                                                                                              */
/******************************************************************************************************************************/
static void Sms_send_to_all_authorized_recipients ( JsonNode *msg )
 { if (Agent_vars->sending_is_disabled == TRUE)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_NOTICE, "Sending is disabled. Dropping message" );
      return;
    }

   gint notif_sms = Json_get_int ( msg, "notif_sms" );
   if (notif_sms == TXT_NOTIF_BY_DLS) notif_sms = Json_get_int ( msg, "notif_sms_by_dls" );

   JsonNode *UsersNode = NULL;
   GList *Recipients = NULL;
   if (Agent_config_get_bool ( Agent, "standalone" ) == FALSE )
    { UsersNode = Http_Get_from_global_API ( Agent, "/run/users/wanna_be_notified", NULL );
      if (!UsersNode || Json_get_int ( UsersNode, "http_code" ) != 200)
       { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "Could not get USERS from API" );
         if (UsersNode) Json_unref ( UsersNode );
         return;
       }
      Recipients = json_array_get_elements ( Json_get_array ( UsersNode, "recipients" ) );
    }
   else
    { Recipients = json_array_get_elements ( Agent_config_get_array ( Agent, "recipients" ) ); }

   for (GList *recipients = Recipients; recipients; recipients = g_list_next(recipients))
    { JsonNode *user = recipients->data;
      gchar *user_phone = Json_get_string ( user, "phone" );
      if (!user_phone)
       { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
               "Warning: User %s does not have a phone number", Json_get_string ( user, "email" ) );
       }
      else if (!strlen(user_phone))
       { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
               "Warning: User %s has an empty phone number", Json_get_string ( user, "email" ) );
       }
      else switch (notif_sms)
       { case TXT_NOTIF_YES:
              if ( Sms_send_gsm ( msg, user_phone ) == FALSE )
               { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "Error sending with GSM" );
                 gchar *free_sms_api_user = Json_get_string ( user, "free_sms_api_user" );
                 if (free_sms_api_user && strlen(free_sms_api_user))
                  { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_INFO, "Sending with FREE API" );
                    Sms_send_freeapi ( msg, user );
                  }
                 else
                  { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_INFO, "Sending with OVH" );
                    Sms_send_ovh ( msg, user_phone );
                  }
               }
              break;
         case TXT_NOTIF_OVH_ONLY:
              Sms_send_ovh ( msg, user_phone );
              break;
       }
    }
   g_list_free ( Recipients );
   Json_unref ( UsersNode );
   Mqtt_Send_CI_pulse ( Agent, Agent_vars->ci_nbr_sms );
 }

/******************************************************************************************************************************/
/* Sms_send_internal_text: Diffuse un message technique genere par l'agent lui-meme                                           */
/* Entrée: le texte, l'acronyme associe et le mode de notification                                                            */
/* Sortie: néant                                                                                                              */
/******************************************************************************************************************************/
static void Sms_send_internal_text ( gchar *texte, gchar *acronyme, gint notif_sms )
 { JsonNode *RootNode = Json_create();
   if (!RootNode) return;
   Json_add_string ( RootNode, "tech_id", Agent_get_tech_id ( Agent ) );
   Json_add_string ( RootNode, "acronyme", acronyme );
   Json_add_string ( RootNode, "libelle", texte );
   Json_add_string ( RootNode, "dls_shortname", Agent_get_tech_id ( Agent ) );
   Json_add_int ( RootNode, "notif_sms", notif_sms );
   Sms_send_to_all_authorized_recipients ( RootNode );
   Json_unref ( RootNode );
 }

/******************************************************************************************************************************/
/* Sms_send_ovh_text: Diffuse un message technique en forcant l'envoi par OVH                                                 */
/* Entrée: le texte a envoyer                                                                                                 */
/* Sortie: néant                                                                                                              */
/******************************************************************************************************************************/
static void Sms_send_ovh_text ( gchar *texte )
 { Sms_send_internal_text ( texte, "TEST_OVH", TXT_NOTIF_OVH_ONLY ); }

/******************************************************************************************************************************/
/* Sms_send_gsm_text: Diffuse un message technique en privilegiant l'envoi par le modem GSM                                   */
/* Entrée: le texte a envoyer                                                                                                 */
/* Sortie: néant                                                                                                              */
/******************************************************************************************************************************/
static void Sms_send_gsm_text ( gchar *texte )
 { Sms_send_internal_text ( texte, "TEST_GSM", TXT_NOTIF_YES ); }

/******************************************************************************************************************************/
/* Sms_process_command: Traite une commande recue par SMS, apres controle des droits de l'expediteur                          */
/* Entrée: le numero de l'expediteur et le texte de la commande                                                               */
/* Sortie: néant                                                                                                              */
/******************************************************************************************************************************/
static void Sms_process_command ( gchar *from, gchar *texte )
 { JsonNode *RootNode = Json_create();
   JsonNode *UserNode = NULL;
   JsonNode *MapNode = NULL;

   if ( RootNode == NULL )
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ALERT, "Memory Error for '%s'", from );
      return;
    }
   Json_add_string ( RootNode, "phone", from );

   UserNode = Http_Post_to_global_API ( Agent, "/run/user/can_send_txt_cde", RootNode );
   Json_unref ( RootNode );
   if (!UserNode || Json_get_int ( UserNode, "http_code" ) != 200)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "Could not get USER from API for '%s'", from );
      goto end;
    }
   if ( !Json_has_member ( UserNode, "email" ) )
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
            "%s is not a known user. Dropping command '%s'...", from, texte );
      goto end;
    }
   if ( !Json_has_member ( UserNode, "can_send_txt_cde" ) || Json_get_bool ( UserNode, "can_send_txt_cde" ) == FALSE )
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_WARNING,
            "%s ('%s') is not allowed to send txt_cde. Dropping command '%s'...",
            from, Json_get_string ( UserNode, "email" ), texte );
      goto end;
    }

   if ( !strcasecmp ( texte, "ping" ) )
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_NOTICE, "Ping received from '%s'. Sending Pong", from );
      Sms_send_gsm_text ( "Pong !" );
      goto end;
    }
   if ( !strcasecmp ( texte, "smsoff" ) )
    { Agent_vars->sending_is_disabled = TRUE;
      Sms_send_gsm_text ( "Sending SMS is off !" );
      Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_NOTICE, "Sending SMS is DISABLED by '%s'", from );
      goto end;
    }
   if ( !strcasecmp ( texte, "smson" ) )
    { Sms_send_gsm_text ( "Sending SMS is on !" );
      Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_NOTICE, "Sending SMS is ENABLED by '%s'", from );
      Agent_vars->sending_is_disabled = FALSE;
      goto end;
    }

   RootNode = Json_create();
   if ( RootNode == NULL )
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "MapNode Error for '%s'", from );
      goto end;
    }
   Json_add_string ( RootNode, "agent_tech_id", "_COMMAND_TEXT" );
   Json_add_string ( RootNode, "agent_acronyme", texte );

   MapNode = Http_Post_to_global_API ( Agent, "/run/mapping/search_txt", RootNode );
   Json_unref ( RootNode );
   if (!MapNode || Json_get_int ( MapNode, "http_code" ) != 200)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "Could not get MAP from API for '%s'", from );
      goto end;
    }
   if ( Json_has_member ( MapNode, "nbr_results" ) == FALSE )
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR, "Error searching database for '%s'", texte );
      Sms_send_gsm_text ( "Error searching Database .. Sorry .." );
      goto end;
    }

   gint nbr_results = Json_get_int ( MapNode, "nbr_results" );
   if ( nbr_results == 0 )
    { Sms_send_gsm_text ( "Je n'ai pas trouve, desole." ); }
   else
    { if ( nbr_results > 1 ) Sms_send_gsm_text ( "Aie, plusieurs choix sont possibles ... :" );

      GList *Results = json_array_get_elements ( Json_get_array ( MapNode, "results" ) );
      if ( nbr_results > 1 )
       { for (GList *results = Results; results; results = g_list_next(results))
          { JsonNode *element = results->data;
            gchar *agent_acronyme = Json_get_string ( element, "agent_acronyme" );
            gchar *tech_id = Json_get_string ( element, "tech_id" );
            gchar *acronyme = Json_get_string ( element, "acronyme" );
            gchar *libelle = Json_get_string ( element, "libelle" );
            Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_INFO,
                  "From '%s' map found for '%s' -> '%s:%s' - %s", from, agent_acronyme, tech_id, acronyme, libelle );
            Sms_send_gsm_text ( agent_acronyme );
          }
       }
      else
       { JsonNode *element = Results->data;
         gchar *agent_acronyme = Json_get_string ( element, "agent_acronyme" );
         gchar *tech_id = Json_get_string ( element, "tech_id" );
         gchar *acronyme = Json_get_string ( element, "acronyme" );
         gchar *libelle = Json_get_string ( element, "libelle" );
         Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_INFO,
               "From '%s' map found for '%s' (%s) -> '%s:%s' - %s",
               from, Json_get_string ( UserNode, "email" ), agent_acronyme, tech_id, acronyme, libelle );
         Mqtt_Send_DI_pulse ( Agent, tech_id, acronyme );
         gchar chaine[256];
         g_snprintf ( chaine, sizeof(chaine), "'%s' fait.", texte );
         Sms_send_gsm_text ( chaine );
       }
      g_list_free ( Results );
    }

end:
   if (MapNode) Json_unref ( MapNode );
   if (UserNode) Json_unref ( UserNode );
 }

/******************************************************************************************************************************/
/* Sms_get_property: Lit une propriete D-Bus d'un objet SMS de ModemManager                                                   */
/* Entrée: la connexion au bus systeme, le chemin de l'objet SMS et le nom de la propriete                                    */
/* Sortie: la valeur de la propriete, NULL si erreur                                                                          */
/******************************************************************************************************************************/
static GVariant *Sms_get_property ( GDBusConnection *system_bus, gchar *sms_path, gchar *property )
 { GError *error = NULL;
   GVariant *reply;
   GVariant *value;

   reply = g_dbus_connection_call_sync ( system_bus, MM_DBUS_SERVICE, sms_path,
                                         MM_DBUS_PROPERTIES_IFACE, "Get",
                                         g_variant_new ( "(ss)", MM_DBUS_SMS_IFACE, property ),
                                         G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
                                         10000, NULL, &error );
   if (!reply)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
            "Cannot read SMS property '%s' (%s)", property, error ? error->message : "unknown" );
      g_clear_error ( &error );
      return(NULL);
    }
   g_variant_get ( reply, "(v)", &value );
   g_variant_unref ( reply );
   return(value);
 }

/******************************************************************************************************************************/
/* Sms_read_gsm: Lit les SMS recus par le modem, les traite puis les supprime                                                 */
/* Entrée: néant                                                                                                              */
/* Sortie: TRUE si la liste des SMS a pu etre parcourue                                                                       */
/******************************************************************************************************************************/
static gboolean Sms_read_gsm ( void )
 { GDBusConnection *system_bus;
   gchar *modem_path = NULL;
   GError *error = NULL;
   GVariant *reply;
   GVariantIter *sms_iter;
   const gchar *sms_path;

   if (!Sms_get_modem_path ( &modem_path )) return(FALSE);
   system_bus = g_bus_get_sync ( G_BUS_TYPE_SYSTEM, NULL, &error );
   if (!system_bus)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_ERR,
            "Cannot connect to system D-Bus (%s)", error ? error->message : "unknown" );
      g_clear_error ( &error );
      g_free ( modem_path );
      return(FALSE);
    }

   reply = g_dbus_connection_call_sync ( system_bus, MM_DBUS_SERVICE, modem_path,
                                         MM_DBUS_MESSAGING_IFACE, "List", NULL,
                                         G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NONE,
                                         10000, NULL, &error );
   if (!reply)
    { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_WARNING,
            "Cannot list SMS (%s)", error ? error->message : "unknown" );
      g_clear_error ( &error );
      g_object_unref ( system_bus );
      g_free ( modem_path );
      return(FALSE);
    }

   g_variant_get ( reply, "(ao)", &sms_iter );
   while ( g_variant_iter_next ( sms_iter, "&o", &sms_path ) )
    { GVariant *state_v = Sms_get_property ( system_bus, (gchar *)sms_path, "State" );
      if (!state_v) continue;
      guint32 state = g_variant_get_uint32 ( state_v );
      g_variant_unref ( state_v );

      if (state == MM_SMS_STATE_RECEIVED)
       { GVariant *number_v = Sms_get_property ( system_bus, (gchar *)sms_path, "Number" );
         GVariant *text_v = Sms_get_property ( system_bus, (gchar *)sms_path, "Text" );
         if (number_v && text_v)
          { const gchar *from = g_variant_get_string ( number_v, NULL );
            const gchar *texte = g_variant_get_string ( text_v, NULL );
            Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_NOTICE,
                  "Recu '%s' from '%s' via %s", texte, from, sms_path );
            Sms_process_command ( (gchar *)from, (gchar *)texte );
          }
         if (number_v) g_variant_unref ( number_v );
         if (text_v) g_variant_unref ( text_v );

         GVariant *delete_reply = g_dbus_connection_call_sync ( system_bus, MM_DBUS_SERVICE, modem_path,
                                                                 MM_DBUS_MESSAGING_IFACE, "Delete",
                                                                 g_variant_new ( "(o)", sms_path ), NULL,
                                                                 G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL );
         if (delete_reply) g_variant_unref ( delete_reply );
       }
    }

   g_variant_iter_free ( sms_iter );
   g_variant_unref ( reply );
   g_object_unref ( system_bus );
   g_free ( modem_path );
   return(TRUE);
 }

/******************************************************************************************************************************/
/* main: Point d'entree de l'agent SMS                                                                                        */
/* Entrée: les parametres de la ligne de commande                                                                             */
/* Sortie: néant                                                                                                              */
/******************************************************************************************************************************/
gint main ( gint argc, gchar *argv[] )
 { Config_add_parameter ( "ovh-service-name",       "SERVICE", "OVH SMS service name", CONFIG_STRING );
   Config_add_parameter ( "ovh-application-key",    "KEY",     "OVH application key", CONFIG_STRING );
   Config_add_parameter ( "ovh-application-secret", "SECRET",  "OVH application secret", CONFIG_STRING );
   Config_add_parameter ( "ovh-consumer-key",       "KEY",     "OVH consumer key", CONFIG_STRING );
   Config_add_parameter ( "read-interval",          "TOP",     "SMS modem polling interval in deciseconds", CONFIG_INT );

   Agent = Agent_init ( argv[0], "sms", ABLS_AGENT_SMS_VERSION, sizeof(struct ABLS_SMS_VARS), argc, argv );
   Agent_vars = Agent_get_vars ( Agent );

   Agent_vars->sending_is_disabled = FALSE;
   Agent_vars->ci_nbr_sms          = Mnemo_create_CI ( Agent, "NBR_SMS", "Nombre de SMS envoyes", "sms", AGENT_ARCHIVE_1_HEURE );
   Agent_vars->ai_signal_quality   = Mnemo_create_AI ( Agent, "SIGNAL_QUALITY", "Qualite du signal", "%", AGENT_ARCHIVE_1_HEURE );

   Agent_subscribe_mqtt_local ( Agent, "SEND_SMS" );

   Agent_is_ready ( Agent );
   Sms_send_gsm_text ( "SMS System is running" );

   guint next_read = 0;
   guint read_interval = Agent_config_get_int ( Agent, "read_interval" );
   if (read_interval <= 0) read_interval = SMS_DEFAULT_READ_INTERVAL;

   while (Agent_is_running ( Agent ))
    { Agent_loop ( Agent );

      JsonNode *mqtt_local_message;
      while ( (mqtt_local_message = Agent_get_mqtt_local_message ( Agent ) ) != NULL )
       { if ( Mqtt_topic_is ( mqtt_local_message, 1, "SEND_SMS" ) &&
               Json_has_member ( mqtt_local_message, "notif_sms" ) &&
               Json_has_member ( mqtt_local_message, "tech_id" ) &&
               Json_has_member ( mqtt_local_message, "acronyme" ) &&
               Json_has_member ( mqtt_local_message, "libelle" ) )
          { Info( __func__, Agent_get_classe ( Agent ), Agent_get_tech_id ( Agent ), LOG_NOTICE,
                  "Sending msg '%s:%s' (%s)",
                  Json_get_string ( mqtt_local_message, "tech_id" ),
                  Json_get_string ( mqtt_local_message, "acronyme" ),
                  Json_get_string ( mqtt_local_message, "libelle" ) );
            Sms_send_to_all_authorized_recipients ( mqtt_local_message );
          }
         Json_unref ( mqtt_local_message );
       }

      JsonNode *mqtt_api_message;
      while ( (mqtt_api_message = Agent_get_mqtt_api_message ( Agent ) ) != NULL )
       { if ( Mqtt_topic_is ( mqtt_api_message, 4, "+", "AGENT", Agent_get_tech_id ( Agent ), "TEST" ) )
          { gchar *test_mode = Json_get_string ( mqtt_api_message, "test_mode" );
            if (test_mode && !strcasecmp ( test_mode, "OVH" )) Sms_send_ovh_text ( "Test SMS OVH OK !" );
            else Sms_send_gsm_text ( "Test SMS GSM OK !" );
          }
         Json_unref ( mqtt_api_message );
       }

      if (Agent_get_top ( Agent ) < next_read) continue;

      gdouble signal_quality;
      if (Sms_get_signal_quality ( &signal_quality ))
       { Agent_send_comm_to_master ( Agent, TRUE );
         Mqtt_Send_AI ( Agent, Agent_vars->ai_signal_quality, signal_quality, TRUE );
         Sms_read_gsm();
       }
      else Agent_send_comm_to_master ( Agent, FALSE );

      next_read = Agent_get_top ( Agent ) + read_interval;
    }

   g_clear_pointer ( &Agent_vars->mm_modem_path, g_free );
   Agent_end ( Agent );
 }
/*----------------------------------------------------------------------------------------------------------------------------*/
