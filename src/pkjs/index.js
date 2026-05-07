// pebbql config flow.
//
// Pebble's settings UI is two JS events:
//   - 'showConfiguration': open a URL the user submits a form on.
//   - 'webviewclosed':     receive whatever the form posted back via
//                          the pebblejs://close#<urlencoded-json> hand-
//                          off, then forward to the watchapp via
//                          AppMessage. Keys here must match messageKeys
//                          in package.json and MESSAGE_KEY_* on the C side.
//
// The form is inlined as a data: URL so we don't have to host anything.
// One toggle: dark vs light. Saved choice is round-tripped through
// localStorage so the form pre-selects what's currently active.

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
+ '.opts{display:flex;gap:.5em;margin:1em 0;}'
+ '.opts label{flex:1;display:block;padding:1em;border:2px solid #ccc;'
+ 'border-radius:8px;text-align:center;font-weight:600;cursor:pointer;'
+ 'background:#fff;}'
+ '.opts input{display:none;}'
+ '.opts input:checked + span{color:#e10098;}'
+ '.opts label:has(input:checked){border-color:#e10098;}'
+ 'button{margin-top:1.8em;width:100%;padding:.85em;background:#e10098;'
+ 'color:#fff;border:0;border-radius:6px;font-size:1em;font-weight:600;}'
+ '</style></head><body>'
+ '<h1>pebbql</h1>'
+ '<p>Pick a theme.</p>'
+ '<form id=f>'
+ '<div class=opts>'
+ '<label><input type=radio name=DARK value=1><span>Dark</span></label>'
+ '<label><input type=radio name=DARK value=0><span>Light</span></label>'
+ '</div>'
+ '<button type=submit>Save</button>'
+ '</form>'
+ '<script>'
+ '(function(){'
+ 'var S=__SAVED__,v="DARK" in S?S.DARK:1;'
+ 'var rs=document.querySelectorAll("input[name=DARK]");'
+ 'for(var i=0;i<rs.length;i++)if(parseInt(rs[i].value,10)===v)rs[i].checked=true;'
+ 'document.getElementById("f").addEventListener("submit",function(e){'
+ 'e.preventDefault();'
+ 'var sel=document.querySelector("input[name=DARK]:checked");'
+ 'var d={DARK:sel?parseInt(sel.value,10):1};'
+ 'document.location="pebblejs://close#"+encodeURIComponent(JSON.stringify(d));});'
+ '})();'
+ '</script></body></html>'
);

Pebble.addEventListener('showConfiguration', function () {
  var saved = '{}';
  try { saved = localStorage.getItem('theme') || '{}'; } catch (_) {}
  var html = CONFIG_HTML.replace('__SAVED__', saved);
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
