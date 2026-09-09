/*
 * Almacen mutable del HTML del portal + actualizacion por downlink LoRa.
 *
 * El HTML vive en un buffer estatico (servido por el HTTP server) y se
 * persiste en Settings/NVS bajo la clave "portal/html". Al arrancar se
 * restaura de NVS; si no hay nada, se usa el default empotrado.
 *
 * La actualizacion por LoRa reensambla en un buffer 'staging' separado, de
 * modo que una transferencia a medias NUNCA corrompe la pagina viva: solo
 * en COMMIT (con longitud y CRC validados) se vuelca a vivo + NVS.
 */
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "portal/portal.h"

LOG_MODULE_REGISTER(portal_html, LOG_LEVEL_INF);

/* Dashboard del portal cautivo. Tres bloques:
 *
 *  1. Aviso de incidencia: los dos telefonos de la Policia Local como enlaces
 *     "tel:". Tocar uno abre el marcador Y pega un GET a /api/aviso/... para
 *     que el servidor se entere (uplink FPort 3 -> ChirpStack -> MQTT). Los
 *     numeros van tambien como texto normal porque el navegador cautivo de
 *     iOS/Android puede ignorar "tel:" en su ventana reducida.
 *
 *  2. Tarjetas de sensores con barras de nivel por zonas de umbral. Codigo de
 *     color unico en toda la pagina, TRES zonas (variable Q del script):
 *       verde #22c55e normal   ambar #f59e0b aviso   rojo #ef4444 alto
 *     Tinen las etiquetas de estado, la palabra de "Gases", los cortes del
 *     degradado de cada barra y los numeros de umbral de la escala.
 *
 *     Hubo una cuarta zona ("peligroso", morado #a855f7, la convencion del
 *     AQI). Se retiro: el morado no encajaba y, sobre una barra de 9 px, un
 *     cuarto matiz por encima del rojo no anadia informacion util -- pasado
 *     el rojo la respuesta de quien mira el panel ya es la misma. OJO: esto
 *     solo toca la ESCALA VISUAL; los umbrales que disparan alertas por LoRa
 *     son los TH_* de main.c y no han cambiado.
 *
 *     Barras y no sparklines: el nodo NO guarda historico, la serie solo vivia
 *     en la memoria del navegador desde que se abria la pagina (arrancaba
 *     vacia en cada visita y se perdia al recargar). La barra con umbrales SI
 *     da contexto a una lectura instantanea. (Los sparklines SVG quedan en el
 *     historial de git.)
 *
 *     NO hay bloque de ayuda. El panel llevaba una leyenda de colores, un
 *     glosario y un "como funciona"; se retiraron TODOS por peticion expresa.
 *     La pagina se explica sola con lo que ya lleva cada barra: la etiqueta de
 *     estado ("Aviso", "Alto"...) y los numeros de umbral bajo la escala, cada
 *     uno pintado del color de la franja que abre. Si algun dia hace falta
 *     recuperar la leyenda, esta en el historial de git.
 *
 * CONFIRMACION DE LLAMADA: tocar un telefono NO marca directamente; abre un
 * dialogo de confirmacion. El portal se abre solo en la pantalla del movil
 * (redirect del captive) y estos botones son grandes y rojos: sin este paso,
 * un roce basta para llamar a la Policia Local y para meter un aviso falso en
 * el sistema. El boton "Llamar" del dialogo es un <a href="tel:"> DE VERDAD,
 * no un boton con location.href: el navegador cautivo de iOS/Android acepta
 * mucho mejor una navegacion nativa por gesto del usuario que una programada.
 *
 * INDICADOR DE LORA: SOLO COLOR, sin una palabra en pantalla. Lo dicen el
 * color del titulo y el puntito que lo acompana:
 *     verde  unido y enviando        ambar  unido pero sin envios recientes
 *     rojo   sin red LoRa            gris   aun sin saberlo
 * Tocando el puntito, el subtitulo cuenta el detalle unos segundos (es la
 * unica via textual, y hay que buscarla: el estado de la radio le importa a
 * quien mantiene el nodo, no al vecino que abre el portal a mirar el aire).
 * El dato viene de "lora"/"lora_ms" en /api/sensors.
 *
 * IMPORTANTE: el panel NO depende de la radio. Los sensores se leen y se
 * muestran aunque el nodo nunca haya llegado a unirse a la red (ver el join
 * no bloqueante en main.c); el indicador solo dice si ademas se estan
 * transmitiendo.
 *
 * 100% autocontenido (sin imagenes ni librerias externas) -> rinde en el
 * navegador cautivo de iOS/Android sin acceso a internet. */
static const char default_html[] =
	"<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>Gandia WildFire</title><style>"
	"*{box-sizing:border-box}body{font-family:system-ui,sans-serif;margin:0;"
	"background:#0f172a;color:#e2e8f0}header{padding:14px;text-align:center}"
	"h1{margin:0;font-size:20px;transition:color .4s}"
	".sub{color:#94a3b8;font-size:12px;margin-top:4px}"
	".wrap{max-width:520px;margin:0 auto;padding:0 12px 24px}"
	/* Estado de la radio EN EL TITULO: es lo primero que se mira y de un
	 * vistazo ya dice si el nodo esta transmitiendo. El puntito lo acompana
	 * (ahora a plena opacidad, ya no es decorativo) y responde al toque. */
	"h1.lon{color:#4ade80}h1.lwarn{color:#fbbf24}h1.loff{color:#f87171}"
	".led{display:inline-block;width:9px;height:9px;border-radius:50%;"
	"background:#475569;vertical-align:middle;margin-left:8px;cursor:pointer;"
	"transition:background .4s}.led.lon{background:#22c55e}"
	".led.lwarn{background:#f59e0b}.led.loff{background:#ef4444}"
	/* Aviso de incidencia: filo rojo, sin gritar como el viejo boton de SOS. */
	".call{background:#1e293b;border-left:4px solid #dc2626;border-radius:12px;"
	"padding:12px 14px;margin:16px 0 10px}"
	".call h2{margin:0 0 5px;font-size:15px;color:#fca5a5}"
	".call p{margin:0 0 11px;font-size:12.5px;color:#cbd5e1;line-height:1.5}"
	".tel{display:flex;gap:9px}"
	".tel a{flex:1;padding:10px 6px;border-radius:10px;text-align:center;"
	"text-decoration:none;font-size:17px;font-weight:700;color:#fff;"
	"background:#dc2626;font-variant-numeric:tabular-nums;line-height:1.25}"
	".tel a:active{background:#991b1b}"
	".tel a.alt{background:transparent;color:#fca5a5;"
	"box-shadow:inset 0 0 0 1.5px #dc2626}"
	".tel a.alt:active{background:#450a0a}"
	".tel s{display:block;font-size:10px;font-weight:600;color:#fecaca;"
	"text-decoration:none;letter-spacing:.04em;margin-top:2px}"
	".tel a.alt s{color:#f87171}.tel s.ok{color:#22c55e}.tel s.ko{color:#fbbf24}"
	".card{background:#1e293b;border-radius:12px;padding:12px 14px;margin:10px 0}"
	".card h2{margin:0 0 8px;font-size:15px;color:#38bdf8;display:flex;"
	"align-items:center;gap:8px}.dot{width:10px;height:10px;border-radius:50%;"
	"background:#475569;display:inline-block}.dot.on{background:#22c55e}"
	".dot.err{background:#ef4444}.grid{display:grid;grid-template-columns:1fr 1fr;"
	"gap:4px 14px}.kv{display:flex;justify-content:space-between;font-size:14px;"
	"padding:2px 0}.kv b{font-variant-numeric:tabular-nums}.kv u{color:#94a3b8;"
	"font-weight:400;font-size:11px;text-decoration:none;margin-left:3px}"
	/* Barra de nivel: la pista es el degradado de zonas (los cortes en % son
	 * los umbrales sobre el fondo de escala) y el cursor marca el valor. */
	".lbl{font-size:11px;color:#94a3b8;margin-top:12px;display:flex;"
	"justify-content:space-between;align-items:center;gap:8px}"
	/* Rango de la escala, junto al nombre: sin el, un valor suelto bajo el
	 * cursor no dice donde cae. */
	".lbl u{color:#64748b;text-decoration:none;margin-left:5px}"
	".tag{font-size:10px;font-weight:700;padding:2px 8px;border-radius:99px;"
	"background:#334155;color:#e2e8f0;white-space:nowrap}"
	/* El hueco de abajo (24px) es para la etiqueta del valor, que va colgada
	 * del cursor y viaja con el. */
	".bar{position:relative;height:9px;border-radius:5px;margin:5px 0 24px}"
	".bar.off{filter:grayscale(1);opacity:.35}"
	".bar i{position:absolute;top:-4px;left:0;margin-left:-2px;width:4px;"
	"height:17px;border-radius:2px;background:#f8fafc;"
	"box-shadow:0 0 0 1.5px #0f172a;transition:left .4s}"
	/* Valor bajo el cursor: MISMO 'left' que el cursor y centrado sobre el
	 * con translateX(-50%), asi queda siempre a plomo con la marca blanca. */
	".bar em{position:absolute;top:16px;left:0;transform:translateX(-50%);"
	"font-style:normal;font-size:11.5px;font-weight:700;color:#f1f5f9;"
	"white-space:nowrap;font-variant-numeric:tabular-nums;"
	"transition:left .4s}"
	".bt{background:linear-gradient(90deg,#22c55e 0 58%,#f59e0b 58% 75%,#ef4444 75%)}"
	/* CO a fondo de escala 50 ppm: 9->18%, 35->70%. Estaba a 150 ppm, y con
	 * un fondo ambiental de <1 ppm el cursor no salia nunca del 6% izquierdo:
	 * la barra no informaba de nada. 50 ppm deja ver de verdad el tramo que
	 * importa (el ZE15-CO llega a 500, pero por encima de 50 la lectura ya
	 * solo dice "evacuar"). */
	".bc{background:linear-gradient(90deg,#22c55e 0 18%,#f59e0b 18% 70%,"
	"#ef4444 70%)}"
	/* PM2.5 a fondo de escala 100 ug/m3: 12->12%, 35->35%. */
	".bp{background:linear-gradient(90deg,#22c55e 0 12%,#f59e0b 12% 35%,"
	"#ef4444 35%)}"
	/* Dialogo de confirmacion de llamada. 'top/right/bottom/left' y no
	 * 'inset': el WebView cautivo de moviles viejos no entiende inset y el
	 * dialogo se quedaria sin cubrir la pantalla. */
	".mod{position:fixed;top:0;right:0;bottom:0;left:0;z-index:9;padding:18px;"
	"background:rgba(2,6,23,.75);display:none;align-items:center;"
	"justify-content:center}.mod.sh{display:flex}"
	".box{background:#1e293b;border-radius:14px;padding:17px 16px 14px;"
	"width:100%;max-width:340px;box-shadow:0 18px 40px rgba(0,0,0,.55)}"
	".box h3{margin:0 0 8px;font-size:16px;color:#fca5a5}"
	".box p{margin:0 0 15px;font-size:13px;color:#cbd5e1;line-height:1.5}"
	".box p b{color:#fff;font-variant-numeric:tabular-nums}"
	".mb{display:flex;gap:9px}"
	".mb a,.mb button{flex:1;display:flex;align-items:center;"
	"justify-content:center;padding:12px 6px;border:0;border-radius:10px;"
	"font:inherit;font-size:14px;font-weight:700;text-decoration:none;"
	"cursor:pointer}"
	".mb .no{background:#334155;color:#e2e8f0}"
	".mb .si{background:#dc2626;color:#fff}.mb .si:active{background:#991b1b}"
	"footer{text-align:center;color:#94a3b8;font-size:11px;padding:12px 8px;"
	"line-height:1.5}footer b{color:#cbd5e1}</style></head><body>"
	"<header><h1 id=\"hd\">Gandia WildFire<i class=\"led\" id=\"ld\"></i></h1>"
	"<div class=\"sub\" id=\"age\">cargando...</div></header>"
	"<div class=\"wrap\">"
	"<div class=\"call\"><h2>&#9742; Comunicar una incidencia</h2>"
	"<p>Para comunicar una incidencia llamar a <b>962878800</b> / <b>092</b> "
	"(Polic&iacute;a Local Gandia).</p><div class=\"tel\">"
	/* El href real se conserva (permite copiar el numero con pulsacion larga),
	   pero el onclick devuelve false: quien marca es el dialogo. */
	"<a href=\"tel:962878800\" onclick=\"return M(this,'policia',"
	"'962878800','962 87 88 00','la Polic&iacute;a Local de Gandia')\">"
	"962 87 88 00<s>POLIC&Iacute;A LOCAL</s></a>"
	"<a class=\"alt\" href=\"tel:092\" onclick=\"return M(this,'urgencias',"
	"'092','092','urgencias')\">092<s>URGENCIAS</s></a>"
	"</div></div>"
	"<div class=\"card\"><h2><span class=\"dot\" id=\"da\"></span>Ambiente</h2>"
	"<div class=\"grid\">"
	"<div class=\"kv\"><span>Temperatura</span><b><span id=\"t\">--</span> &deg;C</b></div>"
	"<div class=\"kv\"><span>Humedad</span><b><span id=\"h\">--</span> %</b></div>"
	"<div class=\"kv\"><span>Presi&oacute;n</span><b><span id=\"p\">--</span> hPa</b></div></div>"
	"<div class=\"lbl\"><span>Temperatura<u>0-60 &deg;C</u></span>"
	"<span class=\"tag\" id=\"qt\">--</span></div>"
	"<div class=\"bar bt\" id=\"gt\"><i></i><em>--</em></div></div>"
	"<div class=\"card\"><h2><span class=\"dot\" id=\"dq\"></span>Calidad del aire</h2>"
	"<div class=\"grid\">"
	"<div class=\"kv\"><span>Mon&oacute;xido CO</span><b><span id=\"co\">--</span> ppm</b></div>"
	"<div class=\"kv\"><span>PM2.5</span><b><span id=\"pm25\">--</span> <u>&micro;g/m&sup3;</u></b></div>"
	"<div class=\"kv\"><span>PM10</span><b><span id=\"pm10\">--</span> <u>&micro;g/m&sup3;</u></b></div>"
	"<div class=\"kv\"><span>PM1.0</span><b><span id=\"pm1\">--</span> <u>&micro;g/m&sup3;</u></b></div>"
	"<div class=\"kv\"><span>PM4.0</span><b><span id=\"pm4\">--</span> <u>&micro;g/m&sup3;</u></b></div>"
	"<div class=\"kv\"><span>COV</span><b><span id=\"voc\">--</span> <u>&iacute;ndice</u></b></div>"
	"<div class=\"kv\"><span>NOx</span><b><span id=\"nox\">--</span> <u>&iacute;ndice</u></b></div>"
	/* El BM688 entrega OHMIOS de un MOX, un numero que no dice nada a quien
	 * abre el portal. Se muestra traducido (ver GS()): palabra + indice. */
	"<div class=\"kv\"><span>Gases</span><b><span id=\"g\">--</span></b></div></div>"
	"<div class=\"lbl\"><span>Mon&oacute;xido CO<u>0-50 ppm</u></span>"
	"<span class=\"tag\" id=\"qc\">--</span></div>"
	"<div class=\"bar bc\" id=\"gc\"><i></i><em>--</em></div>"
	"<div class=\"lbl\"><span>PM2.5<u>0-100 &micro;g/m&sup3;</u></span>"
	"<span class=\"tag\" id=\"qp\">--</span></div>"
	"<div class=\"bar bp\" id=\"gp\"><i></i><em>--</em></div></div>"
	/* Ayuda. La abre gente sin formacion tecnica: cada bloque responde a una
	 * pregunta concreta y va plegado para no tapar los datos. */
	"</div><footer>Dise&ntilde;ado por <b>Gesinen</b> &middot; Hecho en "
	"Espa&ntilde;a<br>Portal cautivo &middot; http://192.168.4.1</footer>"
	/* Dialogo de confirmacion. Vive oculto en el DOM (no se construye al
	   vuelo) y el boton de confirmar es un <a href="tel:"> real, con el href
	   puesto por M(): asi el marcador se abre por navegacion nativa dentro
	   del mismo gesto del usuario, que es lo unico que respetan siempre los
	   navegadores cautivos de iOS/Android. Tocar el fondo = cancelar. */
	"<div class=\"mod\" id=\"mo\" onclick=\"if(event.target==this)C()\">"
	"<div class=\"box\"><h3 id=\"mt\">&iquest;Llamar?</h3>"
	"<p>Se abrir&aacute; el marcador con el n&uacute;mero <b id=\"mn\">"
	"</b> y se avisar&aacute; al sistema de que hay una incidencia.</p>"
	"<div class=\"mb\"><button type=\"button\" class=\"no\" "
	"onclick=\"C()\">Cancelar</button>"
	"<a class=\"si\" id=\"mk\" href=\"#\" onclick=\"return K()\">"
	"S&iacute;, llamar</a></div></div></div>"
	"<script>"
	"function $(i){return document.getElementById(i);}"
	"function S(i,v){$(i).textContent=v;}"
	"function D(i,o){$(i).className='dot'+(o?' on':' err');}"
	/* Colores de zona, en el mismo orden que los umbrales de cada barra. */
	"var Q=['#22c55e','#f59e0b','#ef4444'];"
	"var NT=['Normal','Aviso','Riesgo'];"
	/* CO y PM2.5 comparten nombres de zona; la temperatura usa los suyos. */
	"var NA=['Normal','Aviso','Alto'];"
	"var NG=['Limpio','Regular','Malo'];"
	/* LT: detalle de la radio (solo al tocar el punto). AG: antiguedad del
	 * dato, que es lo unico que el subtitulo muestra por si mismo. */
	"var LT='',AG='';"
	/* A(): avisa al servidor de la incidencia. keepalive deja la peticion en
	 * vuelo aunque la pagina pase a segundo plano al abrirse el marcador. */
	"function A(a,n){var s=a.getElementsByTagName('s')[0],o=s.textContent;"
	"s.className='';s.textContent='AVISANDO...';"
	"fetch('/api/aviso/'+n,{keepalive:true}).then(function(r){"
	"s.className=r.ok?'ok':'ko';"
	"s.textContent=r.ok?'AVISO ENVIADO':'SIN AVISO';}).catch(function(){"
	"s.className='ko';s.textContent='SIN AVISO';});"
	"setTimeout(function(){s.className='';s.textContent=o;},8000);}"
	/* M(): intercepta el toque del telefono y abre la confirmacion. Devuelve
	 * false SIEMPRE: aqui no se marca ni se avisa a nadie todavia. */
	"var P=null;"
	"function M(a,n,tel,num,who){P={a:a,n:n};"
	"S('mt','\u00bfLlamar a '+who+'?');S('mn',num);"
	"$('mk').href='tel:'+tel;$('mo').className='mod sh';return false;}"
	"function C(){$('mo').className='mod';P=null;return false;}"
	/* K(): confirmado. Lanza el aviso al servidor y devuelve true para que el
	 * <a href="tel:"> siga su curso y abra el marcador. */
	"function K(){if(!P)return false;var p=P;C();A(p.a,p.n);return true;}"
	/* L(): estado de la radio -> COLOR del titulo y del punto, nada mas. Ambar
	 * = unido pero sin envio OK en 15 min, mas de lo que tarda un ciclo
	 * normal. Sin LoRa la pagina sigue mostrando sensores: esto solo informa,
	 * y por eso no gasta ni una linea de texto en pantalla. */
	"function L(d){var m=d.lora_ms,k;"
	"if(!d.lora){k='loff';LT='sin red LoRa (los sensores siguen midiendo)';}"
	"else if(m<0){k='lwarn';LT='LoRa unido, sin envios aun';}"
	"else{k=(m<900000)?'lon':'lwarn';"
	"LT='LoRa unido, ultimo envio hace '+Math.round(m/1000)+' s';}"
	"$('ld').className='led '+k;$('hd').className=k;}"
	/* GS(): la resistencia del MOX sube con aire limpio y baja con gases, de
	 * forma LOGARITMICA. Se mapea 5 kOhm (saturado) a 500 kOhm (limpio) sobre
	 * un indice 0-100 con su palabra, para no ensenar ohmios. Escala
	 * ORIENTATIVA: el MOX no esta calibrado y deriva, por eso el firmware no
	 * lo usa para alertas (TH_GAS_EN=0). Si tu unidad se queda siempre en la
	 * misma palabra, ajusta LO/HI a lo que de en reposo. */
	"function GS(r){var e=$('g');"
	"if(!r||r<=0){e.textContent='--';e.style.color='';return;}"
	"var LO=Math.log(5000),HI=Math.log(500000);"
	"var v=Math.round(100*(Math.log(r)-LO)/(HI-LO));"
	"v=Math.max(0,Math.min(100,v));"
	"var k=v>=70?0:(v>=40?1:2);"
	"e.textContent=NG[k]+' ('+v+')';e.style.color=Q[k];}"
	/* G(barra,etiqueta,valor,valor_texto,fondo_escala,umbrales,nombres):
	 * coloca el cursor en la pista y clasifica la lectura por umbrales.
	 * Sin dato -> barra en gris y etiqueta '--'. */
	"function G(id,tg,v,mx,th,nm,un){var e=$(id),k=(v==null||isNaN(v));"
	"var p=k?0:Math.max(0,Math.min(100,v/mx*100));"
	"e.classList.toggle('off',k);"
	/* Cursor y etiqueta comparten el mismo 'left': van solidarios. */
	"e.children[0].style.left=p+'%';"
	"var b=e.children[1];b.style.left=p+'%';"
	"b.textContent=k?'--':v.toFixed(1)+' '+un;"
	"var t=$(tg);"
	"if(k){t.textContent='--';t.style.background='#334155';"
	"t.style.color='#e2e8f0';return;}"
	"var i=0;while(i<th.length&&v>=th[i])i++;"
	"t.textContent=nm[i];t.style.background=Q[i];t.style.color='#0f172a';}"
	"async function u(){try{var d=await(await fetch('/api/sensors')).json();"
	/* Unifica temp/humedad: BM688 preferente, si no SEN65. */
	"var tm=d.bm688?d.temperature:(d.sen65?d.s_temp:null);"
	"var hm=d.bm688?d.humidity:(d.sen65?d.s_hum:null);"
	"D('da',d.bm688||d.sen65);D('dq',d.co||d.sen65||d.bm688);L(d);"
	"S('t',tm!=null?tm.toFixed(1):'--');"
	"S('h',hm!=null?hm.toFixed(1):'--');"
	"S('p',d.bm688?(d.pressure/100).toFixed(1):'--');"
	"GS(d.bm688?d.gas:null);"
	"S('co',d.co?d.co_ppm.toFixed(1):'--');"
	"S('pm25',d.sen65?d.pm2_5.toFixed(1):'--');"
	"S('pm10',d.sen65?d.pm10_0.toFixed(1):'--');"
	"S('pm1',d.sen65?d.pm1_0.toFixed(1):'--');"
	"S('pm4',d.sen65?d.pm4_0.toFixed(1):'--');"
	"S('voc',d.sen65?d.voc:'--');S('nox',d.sen65?d.nox:'--');"
	"G('gt','qt',tm,60,[35,45],NT,'\\u00b0C');"
	"G('gc','qc',d.co?d.co_ppm:null,50,[9,35],NA,'ppm');"
	"G('gp','qp',d.sen65?d.pm2_5:null,100,[12,35],NA,'\\u00b5g/m\\u00b3');"
	"AG=d.age_ms<0?'sin lecturas a\\u00fan':'actualizado hace '"
	"+(d.age_ms/1000).toFixed(0)+' s';S('age',AG);"
	"}catch(e){AG='sin datos';S('age',AG);}}"
	/* Tocar el indicador cuenta el estado de la radio en el subtitulo. */
	"$('ld').onclick=function(){S('age',LT||'sin datos de radio');"
	"setTimeout(function(){S('age',AG);},5000);};"
	"u();setInterval(u,2000);"
	"</script></body></html>";


/* No desbordar los buffers (vivo + staging) al cargar el default. */
BUILD_ASSERT(sizeof(default_html) <= PORTAL_HTML_MAX,
	     "default_html no cabe en PORTAL_HTML_MAX");

/* Buffer vivo (servido) y su longitud. Protegido por html_lock. */
static uint8_t  html_buf[PORTAL_HTML_MAX];
static size_t   html_len;
static struct k_mutex html_lock;

/* ---- Settings backend: clave "portal/html" ------------------------------ */

static int portal_settings_set(const char *name, size_t len,
			       settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "html", NULL)) {
		if (len > PORTAL_HTML_MAX) {
			LOG_WRN("HTML en NVS (%zu) > max (%d), se ignora",
				len, PORTAL_HTML_MAX);
			return -EINVAL;
		}
		k_mutex_lock(&html_lock, K_FOREVER);
		ssize_t r = read_cb(cb_arg, html_buf, PORTAL_HTML_MAX);
		html_len = (r > 0) ? (size_t)r : 0;
		k_mutex_unlock(&html_lock);
		LOG_INF("HTML restaurado de NVS: %zu bytes", html_len);
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(portal, "portal", NULL,
			       portal_settings_set, NULL, NULL);

int portal_html_init(void)
{
	k_mutex_init(&html_lock);

	/* Default empotrado por si NVS no trae nada. */
	html_len = sizeof(default_html) - 1;
	memcpy(html_buf, default_html, html_len);

	/* settings_subsys_init() ya lo invoca lorawan_start(); volver a
	 * llamarlo es idempotente. Cargar solo el subtree "portal". */
	(void)settings_load_subtree("portal");
	return 0;
}

void portal_html_get(const uint8_t **buf, size_t *len)
{
	*buf = html_buf;
	*len = html_len;
}

/* ---- Reensamblado por downlink LoRa ------------------------------------- */

static uint8_t  staging[PORTAL_HTML_MAX];
static size_t   ota_expected_len;
static uint16_t ota_expected_crc;
static size_t   ota_received;       /* bytes recibidos (acumulado)       */
static bool     ota_active;

void portal_html_ota_rx(const uint8_t *data, uint8_t len)
{
	if (len < 1) {
		return;
	}

	switch (data[0]) {
	case 0x01: /* BEGIN */
		if (len < 5) {
			LOG_WRN("OTA BEGIN corto (%u)", len);
			return;
		}
		ota_expected_len = (size_t)data[1] | ((size_t)data[2] << 8);
		ota_expected_crc = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
		if (ota_expected_len == 0 || ota_expected_len > PORTAL_HTML_MAX) {
			LOG_WRN("OTA BEGIN len invalida: %zu", ota_expected_len);
			ota_active = false;
			return;
		}
		memset(staging, 0, sizeof(staging));
		ota_received = 0;
		ota_active = true;
		LOG_INF("OTA BEGIN: len=%zu crc=0x%04x", ota_expected_len,
			ota_expected_crc);
		break;

	case 0x02: /* DATA: [off_lo][off_hi][bytes...] */
		if (!ota_active || len < 3) {
			return;
		}
		{
			size_t off = (size_t)data[1] | ((size_t)data[2] << 8);
			size_t n = len - 3;

			if (off + n > PORTAL_HTML_MAX) {
				LOG_WRN("OTA DATA fuera de rango off=%zu n=%zu",
					off, n);
				return;
			}
			memcpy(&staging[off], &data[3], n);
			ota_received += n;
			LOG_DBG("OTA DATA off=%zu n=%zu (rx=%zu/%zu)", off, n,
				ota_received, ota_expected_len);
		}
		break;

	case 0x03: /* COMMIT */
		if (!ota_active) {
			return;
		}
		ota_active = false;
		if (ota_received != ota_expected_len) {
			LOG_ERR("OTA COMMIT: rx=%zu != esperado=%zu",
				ota_received, ota_expected_len);
			return;
		}
		{
			uint16_t crc = crc16_ccitt(0xFFFF, staging,
						   ota_expected_len);
			if (crc != ota_expected_crc) {
				LOG_ERR("OTA COMMIT: CRC 0x%04x != 0x%04x",
					crc, ota_expected_crc);
				return;
			}
		}
		/* Validado: a vivo + NVS. */
		k_mutex_lock(&html_lock, K_FOREVER);
		memcpy(html_buf, staging, ota_expected_len);
		html_len = ota_expected_len;
		k_mutex_unlock(&html_lock);

		{
			int err = settings_save_one("portal/html", html_buf,
						    html_len);
			if (err) {
				LOG_ERR("OTA persistir NVS err %d", err);
			} else {
				LOG_INF("OTA COMMIT OK: HTML %zu bytes en vivo+NVS",
					html_len);
			}
		}
		break;

	default:
		LOG_WRN("OTA opcode desconocido 0x%02x", data[0]);
		break;
	}
}
