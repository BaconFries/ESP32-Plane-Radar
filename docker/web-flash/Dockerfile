FROM nginx:1.27-alpine

COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY public/ /usr/share/nginx/html/
COPY docker-entrypoint.sh /docker-entrypoint.sh
RUN chmod +x /docker-entrypoint.sh

# Firmware is expected at /usr/share/nginx/html/firmware/plane-radar.bin
# Prefer bind-mounting public/firmware at runtime so updates need no rebuild.

EXPOSE 80
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD pidof nginx >/dev/null || exit 1

ENTRYPOINT ["/docker-entrypoint.sh"]
