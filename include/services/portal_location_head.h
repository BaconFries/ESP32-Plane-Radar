#pragma once

// Injected into WiFiManager <head>. Browser resolves US ZIP → lat/lon via
// Zippopotam.us before the setup form is submitted (phone needs internet).
static const char kPortalLocationHeadHtml[] = R"PRHEAD(
<style>
#pr-loc-status{font-size:12px;color:#8ec8ff;min-height:1.2em;margin:6px 0 8px}
</style>
<script>
(function(){
function field(name){return document.querySelector('[name="'+name+'"]');}
function setStatus(msg){
  var el=document.getElementById('pr-loc-status');
  if(!el){
    var zip=field('radar_zip');
    if(!zip||!zip.parentNode)return;
    el=document.createElement('div');
    el.id='pr-loc-status';
    zip.parentNode.insertBefore(el,zip.nextSibling);
  }
  el.textContent=msg||'';
}
function parseNum(s){var n=parseFloat(s);return isFinite(n)?n:NaN;}
function hasLatLon(){
  var lat=parseNum(field('radar_lat')&&field('radar_lat').value);
  var lon=parseNum(field('radar_lon')&&field('radar_lon').value);
  return isFinite(lat)&&isFinite(lon)&&lat>=-90&&lat<=90&&lon>=-180&&lon<=180;
}
function zipDigits(){
  var el=field('radar_zip');
  if(!el)return '';
  return String(el.value||'').replace(/\D/g,'').slice(0,5);
}
async function resolveLocation(){
  var z=zipDigits();
  // Prefer ZIP whenever a full 5-digit code is entered (overrides lat/lon defaults).
  if(z.length===5){
    setStatus('Looking up ZIP '+z+'…');
    var resp=await fetch('https://api.zippopotam.us/us/'+z);
    if(!resp.ok)throw new Error('ZIP lookup failed ('+resp.status+'). Check internet on this phone/browser.');
    var data=await resp.json();
    var place=(data.places&&data.places[0])||null;
    if(!place)throw new Error('ZIP not found');
    field('radar_lat').value=parseFloat(place['latitude']).toFixed(6);
    field('radar_lon').value=parseFloat(place['longitude']).toFixed(6);
    setStatus('ZIP '+z+' → '+field('radar_lat').value+', '+field('radar_lon').value);
    return true;
  }
  if(hasLatLon())return true;
  setStatus('Enter a 5-digit US ZIP or valid lat/lon.');
  return false;
}
function bind(){
  document.addEventListener('submit',function(ev){
    var form=ev.target;
    if(!form||!form.querySelector||!form.querySelector('[name="radar_lat"]'))return;
    if(form.dataset.prReady==='1')return;
    ev.preventDefault();
    ev.stopPropagation();
    resolveLocation().then(function(ok){
      if(!ok)return;
      form.dataset.prReady='1';
      HTMLFormElement.prototype.submit.call(form);
    }).catch(function(err){
      console.warn(err);
      setStatus((err&&err.message)?err.message:String(err));
    });
  },true);
}
if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',bind);
else bind();
})();
</script>
)PRHEAD";
