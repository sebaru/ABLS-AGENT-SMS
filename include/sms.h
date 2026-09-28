/******************************************************************************************************************************/
/* ABLS-AGENT-SMS/include/sms.h            Declaration de la structure interne de l'agent SMS                                 */
/* Projet Abls-Habitat                   Gestion d'habitat                                                15.09.2026 12:00:00 */
/* Auteur: LEFEVRE Sebastien                                                                                                  */
/******************************************************************************************************************************/
/*
 * sms.h
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

#ifndef _ABLS_SMS_H_
 #define _ABLS_SMS_H_

 #include <abls-agent-libs/abls-agent-libs.h>

 enum
  { TXT_NOTIF_BY_DLS   = -1,
    TXT_NOTIF_NO       = 0,
    TXT_NOTIF_YES      = 1,
    TXT_NOTIF_OVH_ONLY = 2
  };

 struct ABLS_SMS_VARS
  { gboolean sending_is_disabled;
    gchar *mm_modem_path;
    JsonNode *ci_nbr_sms;
    JsonNode *ai_signal_quality;
  };

 extern struct ABLS_AGENT *Agent;
 extern struct ABLS_SMS_VARS *Agent_vars;

#endif
/*----------------------------------------------------------------------------------------------------------------------------*/
