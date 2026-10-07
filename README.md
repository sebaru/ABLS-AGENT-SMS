# abls-agent-sms

Standalone SMS runtime for Abls-Habitat.

## Current implementation status

- Runtime skeleton based on ABLS-AGENT-LIBS
- Facility fixed to `sms`
- Instance identified by `agent_tech_id`
- GSM send and receive via ModemManager on D-Bus
- Fallback to OVH or Free Mobile API when GSM fails
- Configuration is loaded in this order:
  1. Defaults in code
  2. Environment variables (`ABLS_*`)
  3. Common JSON file (`/etc/abls/abls-agent.conf`, or `ABLS_CONFIG_FILE`)
  4. Command-line options
  5. Instance JSON file (`/etc/abls/abls-agent-sms@<tech_id>.conf`), filling only missing keys

Supported SMS features:

- outgoing SMS via GSM modem
- OVH REST fallback
- Free Mobile REST fallback
- incoming commands `ping`, `smsoff`, `smson`
- text command mapping via `/run/mapping/search_txt`

## Command-line options

Use `abls-agent-sms --help` to display the available options. Options with a value
are shown below using the `--option=value` syntax. JSON keys use underscores
instead of hyphens (for example, `--master-hostname` maps to `master_hostname`).

| Option | Purpose |
| --- | --- |
| `--help` | Display help and exit. |
| `--agent-tech-id=TECH_ID` | Instance identifier, required before loading the instance file. |
| `--standalone` | Disable the Abls-Habitat API connection and use local configuration. |
| `--master-hostname=HOSTNAME` | Local MQTT broker hostname, required in standalone mode (port 1883, QoS 1). |
| `--api-url=URL` | API URL in managed mode. |
| `--domain-uuid=UUID` | Domain identifier in managed mode. |
| `--domain-secret=SECRET` | Domain authentication secret in managed mode. |
| `--server-uuid=UUID` | Server identifier in managed mode; generated if missing. |
| `--tps=TPS` | Main loop iterations per second (default: 50). |
| `--dry-run` | Enable the common agent library's dry-run mode; this is not a guarantee that direct GSM/HTTP SMS sends are suppressed. |
| `--save` | Save the resolved local configuration to the instance file and exit; requires root. |
| `--read-interval=TOP` | Modem polling interval in deciseconds (default: 50, or 5 seconds). |
| `--ovh-service-name=SERVICE` | OVH SMS service name. |
| `--ovh-application-key=KEY` | OVH application key. |
| `--ovh-application-secret=SECRET` | OVH application secret. |
| `--ovh-consumer-key=KEY` | OVH consumer key. |

Recipients in standalone mode are configured in the JSON `recipients` array.

## Standalone configuration

The runtime package installs `abls-agent-sms@tech_id.conf.template` in `/etc/abls/`.
Create an instance file from this template (example instance: `SMS1`):

```sh
sudo install -o root -g abls -m 0640 \
  /etc/abls/abls-agent-sms@tech_id.conf.template \
  /etc/abls/abls-agent-sms@SMS1.conf
sudoedit /etc/abls/abls-agent-sms@SMS1.conf
abls-agent-sms --agent-tech-id=SMS1 --standalone --master-hostname=localhost
```

The template explicitly sets `standalone` to `true`; no `api_url`, `domain_uuid`,
`domain_secret` or `server_uuid` is needed. Set `master_hostname` to the local MQTT
broker and replace the example recipient's `phone` with the real destination.
Add further objects to `recipients` for additional destinations.

GSM sending requires a ModemManager modem accessible over the system D-Bus.
Each recipient may optionally provide `free_sms_api_user` and `free_sms_api_key`
for Free Mobile fallback. Leave both empty when unused. The four `ovh_*` fields
are optional; fill all four to enable OVH sending. Normal notifications try GSM,
then Free Mobile if configured for the recipient, otherwise OVH. OVH-only
notifications require OVH credentials.

The template is not loaded directly: the instance file must end in `.conf`.
CLI options override the common configuration; the instance file only supplies
missing keys, so common configuration can override its `standalone`, broker or
recipient settings. The explicit standalone flags in the example avoid enabling
managed mode through the common configuration.

For systemd, after configuring the instance and checking the common configuration:

```sh
sudo systemctl enable --now abls-agent-sms@SMS1.service
```

## Build

```sh
./install_deps.sh
./build.sh
```

## Packaging RPM

```sh
./build_rpm.sh
```

Produces runtime RPM package in `build/`.

## Packaging DEB

```sh
./build_apt.sh --dist bookworm
./build_apt.sh --dist trixie
```

Default target suite is detected from host OS codename (`/etc/os-release`), with `bookworm` fallback.

Useful options:

- `--version-suffix <s>`: override Debian version suffix (example `~trixie`)
- `--no-dist-suffix`: disable automatic `~<suite>` suffix

Produces runtime DEB package and copies normalized artifacts to:

- `build/deb/<suite>/<arch>/`

`build_apt.sh` builds only the native host architecture.

Package signatures are centralized in ABLS-PKGS (both DEB repository metadata and RPM package/repository signatures).

## Release bump + publication

```sh
./bump.sh 1.2.3
```

The release flow:

- tags `v1.2.3` from `trunk`
- merges `trunk` into `main`
- builds RPM + DEB packages
- copies RPM to `../ABLS-PKGS/public/rpms/<arch>/`
- copies DEB to `../ABLS-PKGS/deb-packages/<suite>/<arch>/`

## Container build

```sh
podman build -t abls-agent-sms:dev \
  --build-arg ABLS_LIBS_DEVEL_RPM_URL=<url> \
  --build-arg ABLS_AGENT_LIBS_DEVEL_RPM_URL=<url> \
  --build-arg ABLS_LIBS_RPM_URL=<url> \
  --build-arg ABLS_AGENT_LIBS_RPM_URL=<url> \
  -f Containerfile .
```
