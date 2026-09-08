async function loadVersion() {
  const el = document.getElementById("versionLabel");
  if (!el) return;
  try {
    const res = await fetch("./firmware/VERSION", { cache: "no-store" });
    if (!res.ok) throw new Error("missing");
    const ver = (await res.text()).trim();
    el.textContent = ver ? `Firmware ${ver}` : "Firmware ready";
  } catch (_) {
    el.textContent = "Firmware ready";
  }
}

loadVersion();
