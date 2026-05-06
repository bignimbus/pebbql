// pebbql config flow.
//
// Pebble's settings UI is just two JS events:
//   - 'showConfiguration': open a URL the user submits a form on.
//   - 'webviewclosed':     receive whatever the form posted back via
//                          the pebblejs://close#<urlencoded-json> hand-
//                          off, then forward to the watchapp via
//                          AppMessage. Keys here must match the
//                          messageKeys in package.json and the C-side
//                          MESSAGE_KEY_* identifiers.
//
// The config page itself is inlined as a data: URL so we don't have
// to host anything. Platform is stitched into the HTML at open time
// so the page can hide rhodamine and gray on B&W watches (aplite,
// diorite). Saved settings are persisted to localStorage on submit
// and pre-filled on subsequent opens.

var CONFIG_HTML = (
  '<!DOCTYPE html>'
+ '<html><head><meta charset="utf-8">'
+ '<meta name="viewport" content="width=device-width,initial-scale=1">'
+ '<title>pebbql</title>'
+ '<style>'
+ 'body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;'
+ 'margin:0;padding:1.5em;background:#fafafa;color:#222;}'
+ 'h1{font-size:1.4em;margin:0 0 .25em;}'
+ 'p{color:#666;font-size:.9em;margin:0 0 1.5em;}'
+ 'label{display:block;margin:1em 0 .25em;font-weight:600;font-size:.95em;}'
+ 'select{width:100%;padding:.6em;font-size:1em;border:1px solid #ccc;'
+ 'border-radius:6px;background:#fff;}'
+ 'button{margin-top:1.8em;width:100%;padding:.85em;background:#e10098;'
+ 'color:#fff;border:0;border-radius:6px;font-size:1em;font-weight:600;}'
+ '</style></head><body>'
+ '<h1>pebbql</h1>'
+ '<p>Pick a color for each axis. Rhodamine and gray are unavailable on monochrome watches.</p>'
+ '<form id=f>'
+ '<label>Background<select name=BG></select></label>'
+ '<label>Hexagraph<select name=LOGO></select></label>'
+ '<label>Hour hand<select name=HOUR></select></label>'
+ '<label>Minute hand<select name=MINUTE></select></label>'
+ '<button type=submit>Save</button>'
+ '</form>'
+ '<script>'
+ '(function(){'
+ 'var P="__PLATFORM__",S=__SAVED__,'
+ 'BW=(P==="aplite"||P==="diorite"),'
+ 'C=[["Rhodamine",0,1],["White",1,0],["Black",2,0],["Gray",3,1]],'
+ 'D={BG:2,LOGO:1,HOUR:0,MINUTE:3};'
+ '["BG","LOGO","HOUR","MINUTE"].forEach(function(k){'
+ 'var el=document.querySelector("select[name="+k+"]"),v=k in S?S[k]:D[k];'
+ 'C.forEach(function(c){'
+ 'if(BW&&c[2])return;'
+ 'var o=document.createElement("option");'
+ 'o.value=c[1];o.textContent=c[0];'
+ 'if(c[1]===v)o.selected=true;'
+ 'el.appendChild(o);});});'
+ 'document.getElementById("f").addEventListener("submit",function(e){'
+ 'e.preventDefault();'
+ 'var d={};'
+ '["BG","LOGO","HOUR","MINUTE"].forEach(function(k){'
+ 'd[k]=parseInt(document.querySelector("select[name="+k+"]").value,10);});'
+ 'document.location="pebblejs://close#"+encodeURIComponent(JSON.stringify(d));});'
+ '})();'
+ '</script></body></html>'
);

Pebble.addEventListener('showConfiguration', function () {
  var watch = (Pebble.getActiveWatchInfo && Pebble.getActiveWatchInfo()) || {};
  var platform = watch.platform || 'unknown';
  var saved = '{}';
  try { saved = localStorage.getItem('theme') || '{}'; } catch (_) {}
  var html = CONFIG_HTML
    .replace('__PLATFORM__', platform)
    .replace('__SAVED__', saved);
  Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e.response) return;
  var data;
  try { data = JSON.parse(decodeURIComponent(e.response)); } catch (_) { return; }
  try { localStorage.setItem('theme', JSON.stringify(data)); } catch (_) {}
  Pebble.sendAppMessage(data, function () {}, function (err) {
    console.log('AppMessage failed: ' + JSON.stringify(err));
  });
});
