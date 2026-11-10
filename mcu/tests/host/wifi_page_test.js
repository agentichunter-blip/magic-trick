// Exercise real page event handlers with a minimal DOM and HTTP/XHR doubles.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const html=fs.readFileSync(path.join(__dirname,'../../main/wifi_files.html'),'utf8');
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
class Element {
 constructor(){this.value='';this.hidden=false;this.children=[];this.textContent='';this.classList={add(){},remove(){},toggle(){}};this.listeners={}}
 append(...children){this.children.push(...children)} replaceChildren(...children){this.children=children}
 addEventListener(event,fn){this.listeners[event]=fn} querySelector(){return new Element()}
 showModal(){} close(){} click(){} remove(){} select(){}
}
const elements=new Map([...html.matchAll(/id="([^"]+)"/g)].map(m=>[m[1],new Element()]));
const storage=new Map(),requests=[];let uploads=[],expired=false,scanPolls=0,statusHangs=false;
const context={console,JSON,Error,Array,Number,String,AbortController,clearTimeout(){},setTimeout(fn,delay){if(delay===300 || (delay===8000 && statusHangs))Promise.resolve().then(fn)},setInterval(){},confirm:()=>true,
 document:{getElementById:id=>{assert(elements.has(id),id);return elements.get(id)},createElement:()=>new Element(),body:new Element(),activeElement:null},
 sessionStorage:{getItem:k=>storage.get(k),setItem:(k,v)=>storage.set(k,v),removeItem:k=>storage.delete(k)},
 location:{origin:'http://192.168.4.1',hostname:'192.168.4.1'},navigator:{},window:{addEventListener(){}},
 URL:{createObjectURL:()=>'',revokeObjectURL(){}},
 fetch:async(url,options={})=>{
  requests.push({url,options});
  if(url==='/api/status' && statusHangs)return new Promise((resolve,reject)=>options.signal.addEventListener('abort',()=>reject(Error('aborted'))));
  if(expired)return {ok:false,status:401,text:async()=>'{"error":"Pair again"}'};
  let value=url==='/api/session'?{token:'secret-token'}:url==='/api/status'?{router_connected:false,network_saved:false,message:'Setup'}:{};
  if(url==='/api/networks')value=options.method==='POST'?{scanning:true}:++scanPolls===1?{scanning:true}:{scanning:false,error:'',networks:[{ssid:'Home <img src=x>',rssi:-40,secure:true,supported:true},{ssid:'Cafe',rssi:-60,secure:false,supported:true},{ssid:'WPA3 only',rssi:-70,secure:true,supported:false}]};
  if(url.startsWith('/api/list'))return {ok:true,text:async()=>'{"name":"Folder","directory":true,"size":0}\n'};
  return {ok:true,json:async()=>value,text:async()=>JSON.stringify(value)};
 },
 XMLHttpRequest:class{
  constructor(){this.upload={};this.status=201;this.responseText='{"ok":true}';this.headers={}}
  open(method,url){this.method=method;this.url=url}setRequestHeader(k,v){this.headers[k]=v}
  send(file){uploads.push({file,url:this.url,headers:this.headers});Promise.resolve().then(()=>this.onload())}
  abort(){this.onabort()}
 }
};
vm.createContext(context);vm.runInContext(script,context);
const get=id=>elements.get(id),event={preventDefault(){}};
async function settle(){for(let i=0;i<10;i++)await new Promise(resolve=>setImmediate(resolve))}
(async()=>{
 get('accessCode').value='012345';await get('pairForm').onsubmit(event);
 assert.equal(storage.get('chromagic-token'),'secret-token');assert.equal(get('workspace').hidden,false);
 assert.equal(JSON.parse(requests[0].options.body).code,'012345');
 statusHangs=true;await context.status();
 assert.equal(vm.runInContext('polling',context),false);assert(get('connection').textContent.includes('Connection unavailable'));statusHangs=false;
 get('drop').ondrop({...event,dataTransfer:{items:[],files:[{name:'A & B.gbc',size:42},{name:'Second.gb',size:12}]}});await settle();
 assert.equal(uploads.length,2);assert.equal(uploads[0].headers['X-ChroMagic-Token'],'secret-token');assert(uploads[0].url.includes('A%20%26%20B.gbc'));
 assert(get('notice').textContent.includes('2 file(s) uploaded'));
 get('drop').ondrop({...event,dataTransfer:{items:[{webkitGetAsEntry:()=>({isDirectory:true})}],files:[]}});
 assert(get('notice').textContent.includes('Drop files rather than folders'));
 expired=true;await get('refresh').onclick();assert.equal(get('pairPanel').hidden,false);assert.equal(storage.has('chromagic-token'),false);
 console.log('PASS: browser pairing, drag/drop batch, encoded paths, folder rejection, status timeout recovery, expired token');
})().catch(error=>{console.error(error);process.exitCode=1});
