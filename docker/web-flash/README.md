# Plane Radar — web flash (Docker)

Small nginx container that serves a guided USB installer (ESP Web Tools) and the merged firmware `.bin`. Point HAProxy at it so a remote user only opens a URL.

**Requirement:** the site must be served over **HTTPS** (Web Serial will not work on plain `http://` except `localhost`).

## Quick start

1. Put the merged image in place (name must be `plane-radar.bin`):

```bash
./scripts/merge-firmware.sh -o docker/web-flash/public/firmware/plane-radar.bin
```

2. Set the version shown on the page (optional):

```bash
echo "1.2.0" > docker/web-flash/public/firmware/VERSION
```

3. Run:

```bash
cd docker/web-flash
docker compose up -d --build
```

4. Open `http://127.0.0.1:8098/` locally to smoke-test. For real users, put TLS in front (HAProxy).

### Synology Container Manager

Use a stock `nginx:1.27-alpine` image (no custom build) and bind-mount the static files. Lab files + import JSON:

`/Volumes/docker/planetracker_updater/planetracker-updater-1.json`

Host port **8098** (verified free; **8088** is taken by nagios in HAProxy). Public URL after HAProxy: `https://planeradar.sysops.me/`.

Updating firmware later: replace `public/firmware/plane-radar.bin` (and `VERSION`), no image rebuild needed — the compose file bind-mounts that folder.

## HAProxy

Terminate TLS on HAProxy, proxy HTTP to the container. Example backend (adjust host/port):

```
frontend fe_https
    bind :443 ssl crt /etc/haproxy/certs/flash.example.com.pem
    # ...
    use_backend be_plane_radar_flash if { hdr(host) -i flash.example.com }

backend be_plane_radar_flash
    option forwardfor
    http-request set-header X-Forwarded-Proto https
    server flash1 127.0.0.1:8088 check
```

Send the user: `https://flash.example.com/`

## What the user needs

- Chrome or Edge (desktop), or Chrome on Android — **not Safari / iOS**
- A USB-C data cable
- Hold **BOOT**, tap **RESET**, release **BOOT**, then click **Install**

Wi‑Fi and location settings in flash are usually kept; if the radar center looks wrong after update, they can redo Setup at `http://plane-radar.local`.
