#!/bin/sh
set -eu
VERSION_FILE=/usr/share/nginx/html/firmware/VERSION
MANIFEST=/usr/share/nginx/html/manifest.json
if [ -f "$VERSION_FILE" ]; then
  VER=$(tr -d '[:space:]' < "$VERSION_FILE")
  if [ -n "$VER" ] && [ -f "$MANIFEST" ]; then
    sed -i "s/\"version\": \"[^\"]*\"/\"version\": \"$VER\"/" "$MANIFEST"
  fi
fi
exec nginx -g "daemon off;"
