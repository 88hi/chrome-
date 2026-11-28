#ifndef CHROME_PLUS_SRC_BOOKMARKS_WEBUI_H_
#define CHROME_PLUS_SRC_BOOKMARKS_WEBUI_H_

#include <string>
#include <string_view>

// Self-contained, Trusted-Types-safe multi-column 360-style bookmark UI
// injected into `chrome://bookmarks` (`bookmarks.rollup.js`).
// Split across string chunks to stay well below MSVC's 16,380-byte C2026 limit.
inline std::string GetBookmarksWebUiJs() {
  static constexpr std::string_view kPart1 = R"JS((()=>{
'use strict';
const B=globalThis.chrome&&chrome.bookmarks;
const BM=globalThis.chrome&&chrome.bookmarkManagerPrivate;
if(!B)return;

const rawHash=location.hash||'';
const isPopup=rawHash.startsWith('#cp_popup');
const qs=new URLSearchParams(rawHash.includes('&')?rawHash.slice(rawHash.indexOf('&')+1):'');
const qInt=(k,d,lo,hi)=>{const v=parseInt(qs.get(k)||'',10);return Number.isFinite(v)?Math.max(lo,Math.min(hi,v)):d;};
const qCol=(k,d)=>{const v=(qs.get(k)||'').trim();return /^#[0-9a-fA-F]{6}$/.test(v)?v:d;};

const CFG={
  cw:qInt('cw',236,150,520),
  ih:qInt('ih',28,22,44),
  fs:qInt('fs',13,11,22),
  is:qInt('is',16,12,24),
  cr:qInt('cr',6,0,16),
  th:qInt('th',0,0,3),
  so:qs.get('so')!=='0',
  ob:qInt('ob',2,0,2),
  bg:qCol('bg','#F9F9FA'),
  fg:qCol('fg','#202124'),
  hv:qCol('hv','#E8F0FE'),
};

const BASE_TITLE=isPopup?'★ Chrome++ 书签':'书签管理器 - Chrome++';
document.title=BASE_TITLE;

const signalHide=()=>{
  if(!isPopup)return;
  document.title=BASE_TITLE+' [HIDE]';
  setTimeout(()=>{if(document.title.endsWith('[HIDE]'))document.title=BASE_TITLE;},260);
};

const api=(fn,...args)=>new Promise(res=>{
  if(!fn)return res(undefined);
  let ok=false;
  const done=v=>{if(!ok){ok=true;res(v);}};
  try{
    const r=fn.call(B,...args,v=>{void(chrome.runtime&&chrome.runtime.lastError);done(v);});
    if(r&&typeof r.then==='function')r.then(done,()=>done(undefined));
  }catch{done(undefined);}
});

const mql=window.matchMedia&&window.matchMedia('(prefers-color-scheme: dark)');
const isDark=()=>{
  if(CFG.th===1)return false;
  if(CFG.th===2)return true;
  if(CFG.th===3){
    const r=parseInt(CFG.bg.slice(1,3),16),g=parseInt(CFG.bg.slice(3,5),16),b=parseInt(CFG.bg.slice(5,7),16);
    return r*299+g*587+b*114<128000;
  }
  return !!(mql&&mql.matches);
};

const applyTheme=()=>{
  const dk=isDark(),s=document.documentElement.style;
  s.setProperty('--cw',CFG.cw+'px');
  s.setProperty('--ih',CFG.ih+'px');
  s.setProperty('--fs',CFG.fs+'px');
  s.setProperty('--ico',CFG.is+'px');
  s.setProperty('--rad',CFG.cr+'px');
  if(CFG.th===3){
    s.setProperty('--bg',CFG.bg);s.setProperty('--panel',CFG.bg);
    s.setProperty('--fg',CFG.fg);s.setProperty('--hv',CFG.hv);
  }else if(dk){
    s.setProperty('--bg','#202124');s.setProperty('--panel','#292A2D');
    s.setProperty('--fg','#E8EAED');s.setProperty('--hv','#3C4043');
  }else{
    s.setProperty('--bg','#F8F9FA');s.setProperty('--panel','#FFFFFF');
    s.setProperty('--fg','#202124');s.setProperty('--hv','#E8F0FE');
  }
  s.setProperty('--muted',dk?'#9AA0A6':'#5F6368');
  s.setProperty('--bd',dk?'#3C4043':'#DADCE0');
  s.setProperty('--acc',dk?'#8AB4F8':'#1A73E8');
  s.setProperty('--accbg',dk?'rgba(138,180,248,.18)':'rgba(26,115,232,.13)');
  s.setProperty('--sh',dk?'0 8px 28px rgba(0,0,0,.55)':'0 8px 28px rgba(32,33,36,.18)');
  s.setProperty('--bar',dk?'#292A2D':'#FFFFFF');
  s.setProperty('--inp',dk?'#1E1F22':'#F1F3F4');
  s.setProperty('--del',dk?'#F28B82':'#D93025');
};
applyTheme();
if(mql&&mql.addEventListener)mql.addEventListener('change',applyTheme);

const CSS=`*{box-sizing:border-box;user-select:none}
html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden;background:var(--bg);color:var(--fg);font:var(--fs)/1.35 "Microsoft YaHei UI","Segoe UI",system-ui,sans-serif}
bookmarks-app{display:none!important}
#cpRoot{display:flex;flex-direction:column;width:100%;height:100%;background:var(--bg)}
#cpTop{flex:none;height:40px;padding:0 10px;display:flex;align-items:center;gap:8px;background:var(--bar);border-bottom:1px solid var(--bd)}
#cpSearch{flex:1;min-width:120px;max-width:340px;height:28px;padding:0 10px;border:1px solid var(--bd);border-radius:14px;background:var(--inp);color:var(--fg);font:inherit;outline:none;user-select:text}
#cpSearch:focus{border-color:var(--acc);background:var(--panel)}
.cp-btn{height:26px;padding:0 10px;border:1px solid var(--bd);border-radius:6px;background:transparent;color:var(--fg);font-size:12px;cursor:pointer;display:inline-flex;align-items:center;gap:4px;white-space:nowrap}
.cp-btn:hover{background:var(--hv);border-color:var(--acc);color:var(--acc)}
.cp-btn.on{background:var(--accbg);border-color:var(--acc);color:var(--acc);font-weight:600}
.cp-sp{flex:1}
.cp-hint{font-size:11px;color:var(--muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#cpClose{width:26px;padding:0;justify-content:center;font-size:15px}
#cpMain{position:relative;flex:1;min-height:0;overflow-x:auto;overflow-y:hidden;padding:8px}
#cpCols{height:100%;display:flex;flex-direction:column;flex-wrap:wrap;align-content:flex-start;column-gap:6px;row-gap:1px}
.cp-item{width:var(--cw);height:var(--ih);padding:0 8px;border-radius:var(--rad);display:flex;align-items:center;gap:7px;cursor:pointer;position:relative;flex:none;transition:background .08s}
.cp-item:hover,.cp-item.open{background:var(--hv)}
.cp-item.drop-in{background:var(--accbg)!important;outline:2px solid var(--acc);outline-offset:-2px}
.cp-item.drop-before::before,.cp-item.drop-after::after{content:"";position:absolute;left:6px;right:6px;height:2px;background:var(--acc);border-radius:2px;pointer-events:none;z-index:5}
.cp-item.drop-before::before{top:0}
.cp-item.drop-after::after{bottom:0}
.cp-item.drag-src{opacity:.4}
.cp-ico{width:var(--ico);height:var(--ico);flex:none;display:inline-flex;align-items:center;justify-content:center}
.cp-ico img{width:var(--ico);height:var(--ico);display:block;border-radius:2px}
.cp-fb{width:var(--ico);height:var(--ico);border-radius:50%;border:1.5px solid var(--muted);opacity:.65}
.cp-fld{width:var(--ico);height:calc(var(--ico)*.82);border-radius:2px 3px 3px 3px;background:#F6C343;border:1px solid #D49B1A;position:relative;margin-top:2px}
.cp-fld::before{content:"";position:absolute;left:-1px;top:-3px;width:46%;height:3px;background:#F6C343;border:1px solid #D49B1A;border-bottom:none;border-radius:2px 2px 0 0}
.cp-ttl{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.cp-cnt{font-size:11px;color:var(--muted);flex:none}
.cp-arr{font-size:11px;color:var(--muted);flex:none;margin-left:2px}
.cp-sep{width:var(--cw);height:22px;padding:0 8px;display:flex;align-items:center;font-size:11px;font-weight:600;color:var(--muted);border-bottom:1px solid var(--bd);margin:4px 0 2px;flex:none}
.cp-empty{padding:20px;color:var(--muted);font-size:13px;white-space:nowrap}
.cp-fly{position:fixed;z-index:40;background:var(--panel);border:1px solid var(--bd);border-radius:calc(var(--rad) + 2px);box-shadow:var(--sh);display:flex;flex-direction:column;max-height:calc(100vh - 54px);overflow:hidden}
.cp-fly-hd{flex:none;height:30px;padding:0 8px 0 10px;border-bottom:1px solid var(--bd);display:flex;align-items:center;gap:6px;background:var(--bar);font-size:12px;font-weight:600}
.cp-fly-hd button{height:20px;padding:0 6px;border:1px solid var(--bd);border-radius:4px;background:transparent;color:var(--fg);font-size:11px;cursor:pointer}
.cp-fly-hd button:hover{border-color:var(--acc);color:var(--acc);background:var(--hv)}
.cp-fly-bd{flex:1;min-height:0;overflow:auto;padding:5px;display:flex;flex-direction:column;flex-wrap:wrap;align-content:flex-start;column-gap:4px;row-gap:1px}
#cpMenu{position:fixed;z-index:80;min-width:176px;padding:4px;background:var(--panel);border:1px solid var(--bd);border-radius:8px;box-shadow:var(--sh);display:none;flex-direction:column}
#cpMenu.show{display:flex}
.cp-mi{height:28px;padding:0 10px;border:none;border-radius:5px;background:transparent;color:var(--fg);font:inherit;font-size:12.5px;text-align:left;cursor:pointer;display:flex;align-items:center;justify-content:space-between;gap:12px}
.cp-mi:hover{background:var(--hv)}
.cp-mi.del{color:var(--del)}
.cp-msep{height:1px;background:var(--bd);margin:4px 6px}
#cpBadge{position:fixed;z-index:95;pointer-events:none;display:none;padding:5px 11px;border-radius:999px;font-size:12px;font-weight:500;background:var(--panel);color:var(--fg);border:1px solid var(--bd);box-shadow:var(--sh);white-space:nowrap}
#cpBadge.super{background:var(--acc);color:#fff;border-color:var(--acc)}
#cpMask{position:fixed;inset:0;z-index:90;background:rgba(0,0,0,.35);display:none;align-items:center;justify-content:center}
#cpMask.show{display:flex}
#cpDlg{width:min(420px,calc(100vw - 32px));background:var(--panel);border:1px solid var(--bd);border-radius:10px;box-shadow:var(--sh);padding:16px;display:flex;flex-direction:column;gap:10px}
#cpDlg h3{margin:0;font-size:15px}
#cpDlg label{display:flex;flex-direction:column;gap:4px;font-size:12px;color:var(--muted)}
#cpDlg input{height:32px;padding:0 10px;border:1px solid var(--bd);border-radius:6px;background:var(--inp);color:var(--fg);font:inherit;outline:none;user-select:text}
#cpDlg input:focus{border-color:var(--acc);background:var(--panel)}
#cpDlgMsg{font-size:13px;line-height:1.5}
.cp-act{display:flex;justify-content:flex-end;gap:8px;margin-top:4px}
.cp-act button{height:30px;padding:0 14px;border-radius:6px;border:1px solid var(--bd);background:transparent;color:var(--fg);cursor:pointer;font-size:13px}
.cp-act button.pri{background:var(--acc);color:#fff;border-color:var(--acc)}
.cp-act button.del{background:var(--del);color:#fff;border-color:var(--del)}
#cpToast{position:fixed;left:50%;bottom:14px;transform:translateX(-50%);z-index:99;padding:6px 14px;border-radius:8px;background:rgba(32,33,36,.92);color:#fff;font-size:12px;opacity:0;pointer-events:none;transition:opacity .18s;max-width:80vw;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#cpToast.show{opacity:1}
body.cp-drag *{cursor:grabbing!important}`;
)JS";

  static constexpr std::string_view kPart2 = R"JS(
const el=(t,c,x)=>{const n=document.createElement(t);if(c)n.className=c;if(x!==undefined)n.textContent=x;return n;};
const on=(t,e,f,o)=>t.addEventListener(e,f,o);

const st=el('style');st.textContent=CSS;document.head.appendChild(st);
const root=el('div');root.id='cpRoot';
const top=el('div');top.id='cpTop';
const inp=el('input');inp.id='cpSearch';inp.type='search';inp.placeholder='搜索书签（标题 / 网址）...';
const bBar=el('button','cp-btn on','书签栏');
const bAll=el('button','cp-btn','全部');
const bAddB=el('button','cp-btn','+ 书签');
const bAddF=el('button','cp-btn','+ 文件夹');
const hint=el('span','cp-hint','拖拽排序 / 拖出后台连续打开 / 右键管理');
const sp=el('div','cp-sp');
top.append(inp,bBar,bAll,bAddB,bAddF,sp,hint);
if(isPopup){
  const cb=el('button','cp-btn','×');cb.id='cpClose';cb.title='关闭 (Esc)';
  on(cb,'click',signalHide);top.appendChild(cb);
}

const main=el('div');main.id='cpMain';
const cols=el('div');cols.id='cpCols';main.appendChild(cols);
const menu=el('div');menu.id='cpMenu';
const badge=el('div');badge.id='cpBadge';
const mask=el('div');mask.id='cpMask';
const toast=el('div');toast.id='cpToast';
root.append(top,main);
document.body.textContent='';
document.body.append(root,menu,badge,mask,toast);

on(main,'wheel',e=>{
  if(Math.abs(e.deltaY)>Math.abs(e.deltaX)&&main.scrollWidth>main.clientWidth){
    e.preventDefault();main.scrollLeft+=e.deltaY;
  }
},{passive:false});
on(document,'dragstart',e=>e.preventDefault());

let tmrToast=0;
const tip=m=>{toast.textContent=m;toast.classList.add('show');clearTimeout(tmrToast);tmrToast=setTimeout(()=>toast.classList.remove('show'),1800);};

let mode='bar',query='',tree=[],byId=new Map(),barRoots=[],otherRoots=[],primaryBar='1';
let flyouts=[],tOpen=0,tClose=0,modal=false,tReload=0;
const dg={down:false,on:false,node:null,el:null,x:0,y:0,act:'none',tgt:null,tgtEl:null,fid:''};

const isFld=n=>!!(n&&!n.url&&Array.isArray(n.children));
const hasDesc=(a,id)=>{
  if(!a||!a.children)return false;
  for(const c of a.children){if(c.id===id||hasDesc(c,id))return true;}
  return false;
};
const allUrls=(f,o=[])=>{
  for(const c of(f&&f.children)||[]){if(c.url)o.push(c);else if(c.children)allUrls(c,o);}
  return o;
};
const indexAll=list=>{for(const n of list||[]){byId.set(n.id,n);if(n.children)indexAll(n.children);}};

const openBm=(n,active)=>{
  if(!n||!n.url)return;
  try{
    if(BM&&BM.openInNewTab)BM.openInNewTab(n.id,!!active);
    else window.open(n.url,'_blank','noopener');
  }catch{window.open(n.url,'_blank','noopener');}
  if(active)signalHide();
  else tip('已在后台打开：'+(n.title||n.url));
};

const mkIco=n=>{
  const w=el('span','cp-ico');
  if(isFld(n)){w.appendChild(el('span','cp-fld'));return w;}
  const img=el('img');img.loading='lazy';img.decoding='async';
  img.src='chrome://favicon2/?size='+CFG.is+'&scaleFactor=1x&pageUrl='+encodeURIComponent(n.url||'')+'&allowGoogleServerFallback=0';
  on(img,'error',()=>{w.textContent='';w.appendChild(el('span','cp-fb'));});
  w.appendChild(img);return w;
};

const closeFlyFrom=idx=>{
  while(flyouts.length>idx){
    const f=flyouts.pop();if(f&&f.el&&f.el.parentNode)f.el.parentNode.removeChild(f.el);
  }
  document.querySelectorAll('.cp-item.open').forEach(x=>{
    if(!flyouts.some(f=>f.id===x.dataset.id))x.classList.remove('open');
  });
};
const clrHover=()=>{clearTimeout(tOpen);clearTimeout(tClose);tOpen=tClose=0;};
const schedClose=idx=>{
  clearTimeout(tClose);
  tClose=setTimeout(()=>{if(!dg.on&&!menu.classList.contains('show')&&!modal)closeFlyFrom(idx);},240);
};

const openFly=(fn,lvl,rc)=>{
  if(!fn||!isFld(fn))return;
  if(flyouts[lvl]&&flyouts[lvl].id===fn.id){closeFlyFrom(lvl+1);return;}
  closeFlyFrom(lvl);
  const p=el('div','cp-fly');
  const hd=el('div','cp-fly-hd');
  const tt=el('span','cp-ttl',fn.title||'文件夹');
  const ab=el('button','','+ 书签'),af=el('button','','+ 夹');
  on(ab,'click',e=>{e.stopPropagation();openEdit({m:'add-bm',pid:fn.id});});
  on(af,'click',e=>{e.stopPropagation();openEdit({m:'add-fld',pid:fn.id});});
  hd.append(tt,ab,af);

  const bd=el('div','cp-fly-bd');bd.dataset.emptyFld=fn.id;
  const kids=fn.children||[];
  const rows=Math.max(6,Math.floor((window.innerHeight-104)/(CFG.ih+1)));
  const nc=Math.max(1,Math.min(3,Math.ceil(kids.length/rows)));
  const pw=Math.min(window.innerWidth-24,nc*CFG.cw+(nc-1)*4+14);
  const ph=Math.min(window.innerHeight-54,Math.max(1,Math.min(kids.length,rows))*(CFG.ih+1)+44);
  bd.style.height=(ph-32)+'px';p.style.width=pw+'px';
  if(!kids.length)bd.appendChild(el('div','cp-empty','（空文件夹）'));
  else for(const c of kids)bd.appendChild(mkRow(c,lvl+1));
  on(bd,'contextmenu',e=>{
    if(e.target===bd||e.target.classList.contains('cp-empty')){
      e.preventDefault();showMenu(e.clientX,e.clientY,null,fn.id);
    }
  });
  p.append(hd,bd);
  on(p,'pointerenter',()=>clearTimeout(tClose));
  on(p,'pointerleave',()=>{if(!dg.on)schedClose(lvl);});
  document.body.appendChild(p);

  let lx=rc.right+4;if(lx+pw>window.innerWidth-8)lx=Math.max(8,rc.left-pw-4);
  let ty=rc.top-4;if(ty+ph>window.innerHeight-8)ty=Math.max(44,window.innerHeight-ph-8);
  p.style.left=lx+'px';p.style.top=ty+'px';
  flyouts.push({id:fn.id,el:p,rc});
  document.querySelectorAll('.cp-item[data-id="'+CSS.escape(fn.id)+'"]').forEach(x=>x.classList.add('open'));
};

const mkRow=(n,lvl)=>{
  const r=el('div','cp-item');
  r.dataset.id=n.id;r.dataset.lvl=String(lvl);
  r.title=n.url?(n.title?n.title+'\n'+n.url:n.url):(n.title||'');
  r.append(mkIco(n),el('span','cp-ttl',n.title||n.url||'（无标题）'));
  if(isFld(n)){
    const cnt=(n.children&&n.children.length)||0;
    if(cnt>0)r.appendChild(el('span','cp-cnt',String(cnt)));
    r.appendChild(el('span','cp-arr','▸'));
  }
  on(r,'pointerenter',()=>{
    if(dg.on||menu.classList.contains('show')||modal)return;
    clrHover();
    if(isFld(n))tOpen=setTimeout(()=>openFly(n,lvl,r.getBoundingClientRect()),110);
    else if(flyouts.length>lvl)schedClose(lvl);
  });
  on(r,'pointerdown',e=>{
    if(e.button===1&&n.url){e.preventDefault();openBm(n,false);return;}
    if(e.button!==0)return;
    if(menu.classList.contains('show'))hideMenu();
    dg.down=true;dg.on=false;dg.node=n;dg.el=r;dg.x=e.clientX;dg.y=e.clientY;
    dg.act='none';dg.tgt=null;dg.tgtEl=null;dg.fid='';
  });
  on(r,'contextmenu',e=>{
    e.preventDefault();e.stopPropagation();
    showMenu(e.clientX,e.clientY,n,n.parentId||primaryBar);
  });
  return r;
};
)JS";

  static constexpr std::string_view kPart3 = R"JS(
const clrDrop=()=>{
  if(dg.tgtEl){dg.tgtEl.classList.remove('drop-before','drop-after','drop-in');dg.tgtEl=null;}
  dg.act='none';dg.tgt=null;dg.fid='';
};
const stopDrag=()=>{
  clrDrop();
  if(dg.el)dg.el.classList.remove('drag-src');
  document.body.classList.remove('cp-drag');
  badge.style.display='none';badge.classList.remove('super');
  dg.down=false;dg.on=false;dg.node=null;dg.el=null;
};

on(window,'pointermove',e=>{
  if(!dg.down||!dg.node)return;
  if(!dg.on){
    if(Math.hypot(e.clientX-dg.x,e.clientY-dg.y)<8)return;
    dg.on=true;clrHover();document.body.classList.add('cp-drag');
    if(dg.el){
      dg.el.classList.add('drag-src');
      try{dg.el.setPointerCapture(e.pointerId);}catch{}
    }
  }
  badge.style.display='block';
  badge.style.left=Math.min(window.innerWidth-160,e.clientX+14)+'px';
  badge.style.top=Math.min(window.innerHeight-36,e.clientY+14)+'px';
  clrDrop();

  const out=e.clientX<4||e.clientY<4||e.clientX>window.innerWidth-4||e.clientY>window.innerHeight-4;
  const hit=out?null:document.elementFromPoint(e.clientX,e.clientY);
  const hr=hit&&hit.closest?hit.closest('.cp-item'):null;
  const he=hit&&hit.closest?hit.closest('[data-empty-fld]'):null;

  if(hr&&hr.dataset.id){
    const tgt=byId.get(hr.dataset.id);
    const prot=tgt&&(!tgt.parentId||tgt.parentId==='0');
    if(tgt&&tgt.id!==dg.node.id&&!hasDesc(dg.node,tgt.id)){
      const rc=hr.getBoundingClientRect();
      const ry=(e.clientY-rc.top)/Math.max(1,rc.height);
      if(isFld(tgt)&&(prot||(ry>=0.25&&ry<=0.75))){
        dg.act='into';dg.tgt=tgt;dg.fid=tgt.id;dg.tgtEl=hr;hr.classList.add('drop-in');
        badge.classList.remove('super');badge.textContent='移入「'+(tgt.title||'文件夹')+'」';
        return;
      }
      if(!prot&&!query){
        dg.act=ry<0.5?'before':'after';dg.tgt=tgt;dg.tgtEl=hr;
        hr.classList.add(dg.act==='before'?'drop-before':'drop-after');
        badge.classList.remove('super');
        badge.textContent=(dg.act==='before'?'↑ 移到上方：':'↓ 移到下方：')+(dg.node.title||'');
        return;
      }
    }
    if(tgt&&tgt.id===dg.node.id){
      badge.classList.remove('super');badge.textContent=dg.node.title||'拖拽中...';return;
    }
  }
  if(he&&he.dataset.emptyFld){
    const fid=he.dataset.emptyFld;
    if(fid!==dg.node.id&&!hasDesc(dg.node,fid)){
      const fn=byId.get(fid);
      if(fn&&isFld(fn)){
        dg.act='into';dg.fid=fid;badge.classList.remove('super');
        badge.textContent='移入「'+(fn.title||'文件夹')+'」';return;
      }
    }
  }
  if(dg.node.url){
    dg.act='super';badge.classList.add('super');badge.textContent='↗ 后台新标签页打开（留在当前页）';
  }else{
    badge.classList.remove('super');badge.textContent='拖到目标位置排序';
  }
});

on(window,'pointerup',async e=>{
  if(!dg.down||!dg.node)return;
  const n=dg.node,was=dg.on,act=dg.act,tgt=dg.tgt,fid=dg.fid,srcEl=dg.el;
  stopDrag();
  if(!was){
    if(isFld(n)){
      const lvl=parseInt((srcEl&&srcEl.dataset.lvl)||'0',10)||0;
      if(srcEl)openFly(n,lvl,srcEl.getBoundingClientRect());
    }else if(n.url){
      const fg=e.shiftKey?true:(e.ctrlKey?false:(CFG.ob!==2));
      openBm(n,fg);
    }
    return;
  }
  if(act==='super'&&n.url){openBm(n,false);return;}
  if(act==='into'&&fid){
    if(n.id!==fid&&!hasDesc(n,fid)){
      await api(B.move,n.id,{parentId:fid});tip('已移入文件夹并同步');
    }
    return;
  }
  if((act==='before'||act==='after')&&tgt&&tgt.parentId){
    const p=byId.get(tgt.parentId),kids=(p&&p.children)||[];
    const ti=kids.findIndex(c=>c.id===tgt.id);if(ti<0)return;
    const di=act==='after'?ti+1:ti;
    if(n.parentId===tgt.parentId){
      const ci=kids.findIndex(c=>c.id===n.id);
      if(ci===di||ci+1===di)return;
    }
    await api(B.move,n.id,{parentId:tgt.parentId,index:di});
    tip('排序已保存并同步');
  }
});
on(window,'pointercancel',stopDrag);

const hideMenu=()=>{menu.classList.remove('show');menu.textContent='';};
const mi=(lbl,fn,del=false)=>{
  const b=el('button','cp-mi'+(del?' del':''),lbl);
  on(b,'click',e=>{e.stopPropagation();hideMenu();fn();});
  menu.appendChild(b);
};
const msep=()=>menu.appendChild(el('div','cp-msep'));

const showMenu=(x,y,n,cid)=>{
  hideMenu();
  const prot=n&&(!n.parentId||n.parentId==='0');
  const pid=(n&&!prot&&n.parentId)?n.parentId:(cid||primaryBar);
  const insIdx=()=>{
    if(!n||prot)return undefined;
    const p=byId.get(pid);if(!p||!p.children)return undefined;
    const i=p.children.findIndex(c=>c.id===n.id);return i>=0?i+1:undefined;
  };
  if(n&&!isFld(n)){
    mi('在后台新标签页打开',()=>openBm(n,false));
    mi('在前台新标签页打开',()=>openBm(n,true));
    mi('复制网址',()=>{navigator.clipboard&&navigator.clipboard.writeText(n.url||'').catch(()=>{});tip('已复制网址');});
    msep();
    mi('编辑书签...',()=>openEdit({m:'edit-bm',n}));
    mi('在此新建书签...',()=>openEdit({m:'add-bm',pid,idx:insIdx()}));
    mi('在此新建文件夹...',()=>openEdit({m:'add-fld',pid,idx:insIdx()}));
    msep();
    mi('删除书签',async()=>{await api(B.remove,n.id);tip('已删除并同步');},true);
  }else if(n&&isFld(n)){
    const us=allUrls(n);
    if(us.length){mi('后台打开全部 ('+us.length+')',()=>{for(const u of us)openBm(u,false);});msep();}
    mi('文件夹内新建书签...',()=>openEdit({m:'add-bm',pid:n.id}));
    mi('文件夹内新建子文件夹...',()=>openEdit({m:'add-fld',pid:n.id}));
    if(!prot){
      msep();
      mi('重命名文件夹...',()=>openEdit({m:'edit-fld',n}));
      mi('在此新建书签...',()=>openEdit({m:'add-bm',pid,idx:insIdx()}));
      mi('在此新建文件夹...',()=>openEdit({m:'add-fld',pid,idx:insIdx()}));
      msep();
      mi('删除文件夹',()=>{
        const cnt=(n.children&&n.children.length)||0;
        const go=async()=>{await api(B.removeTree,n.id);tip('已删除文件夹并同步');};
        if(cnt>0)openConfirm('删除文件夹','确定要删除文件夹「'+(n.title||'')+'」及其中的 '+cnt+' 个项目吗？',go);
        else go();
      },true);
    }
  }else{
    mi('新建书签...',()=>openEdit({m:'add-bm',pid}));
    mi('新建文件夹...',()=>openEdit({m:'add-fld',pid}));
    msep();
    mi('刷新书签',()=>reload());
  }
  menu.classList.add('show');
  const mw=menu.offsetWidth||184,mh=menu.offsetHeight||160;
  menu.style.left=Math.max(6,Math.min(window.innerWidth-mw-6,x))+'px';
  menu.style.top=Math.max(6,Math.min(window.innerHeight-mh-6,y))+'px';
};

const closeDlg=()=>{modal=false;mask.classList.remove('show');mask.textContent='';};
const openEdit=({m,n,pid,idx})=>{
  closeDlg();modal=true;
  const isUrl=m==='edit-bm'||m==='add-bm';
  const hd={'edit-bm':'编辑书签','edit-fld':'重命名文件夹','add-bm':'新建书签','add-fld':'新建文件夹'}[m]||'编辑';
  const d=el('div');d.id='cpDlg';d.appendChild(el('h3','',hd));
  const lt=el('label','','名称'),it=el('input');it.type='text';
  it.value=n?(n.title||''):(isUrl?'':'新建文件夹');
  lt.appendChild(it);d.appendChild(lt);
  let iu=null;
  if(isUrl){
    const lu=el('label','','网址');iu=el('input');iu.type='text';
    iu.value=n?(n.url||''):'https://';lu.appendChild(iu);d.appendChild(lu);
  }
  const act=el('div','cp-act'),bc=el('button','','取消'),bo=el('button','pri','确定');
  const sub=async()=>{
    const title=it.value.trim(),url=iu?iu.value.trim():'';
    if(!isUrl&&!title)return;if(isUrl&&!url)return;
    closeDlg();
    if(m==='edit-bm'&&n){await api(B.update,n.id,{title:title||url,url});tip('已更新并同步');}
    else if(m==='edit-fld'&&n){await api(B.update,n.id,{title});tip('已重命名并同步');}
    else if(m==='add-bm'){const s={parentId:pid||primaryBar,title:title||url,url};if(typeof idx==='number')s.index=idx;await api(B.create,s);tip('已添加并同步');}
    else if(m==='add-fld'){const s={parentId:pid||primaryBar,title};if(typeof idx==='number')s.index=idx;await api(B.create,s);tip('已新建文件夹并同步');}
  };
  on(bc,'click',closeDlg);on(bo,'click',sub);
  on(d,'keydown',e=>{if(e.key==='Enter'){e.preventDefault();sub();}else if(e.key==='Escape'){e.preventDefault();e.stopPropagation();closeDlg();}});
  act.append(bc,bo);d.appendChild(act);mask.appendChild(d);mask.classList.add('show');
  setTimeout(()=>{it.focus();it.select();},0);
};
const openConfirm=(t,msg,fn)=>{
  closeDlg();modal=true;
  const d=el('div');d.id='cpDlg';d.appendChild(el('h3','',t));
  const p=el('div','',msg);p.id='cpDlgMsg';d.appendChild(p);
  const act=el('div','cp-act'),bc=el('button','','取消'),bo=el('button','del','确定删除');
  on(bc,'click',closeDlg);on(bo,'click',()=>{closeDlg();fn();});
  act.append(bc,bo);d.appendChild(act);mask.appendChild(d);mask.classList.add('show');bo.focus();
};
on(mask,'click',e=>{if(e.target===mask)closeDlg();});

const render=()=>{
  const prev=flyouts.map(f=>({id:f.id,rc:f.rc}));
  closeFlyFrom(0);cols.textContent='';
  const q=query.trim().toLowerCase();
  if(q){
    let cnt=0;
    for(const n of byId.values()){
      if(!n.parentId||n.parentId==='0')continue;
      if((n.title||'').toLowerCase().includes(q)||(n.url||'').toLowerCase().includes(q)){
        cols.appendChild(mkRow(n,0));cnt++;
      }
    }
    if(!cnt)cols.appendChild(el('div','cp-empty','未找到匹配的书签'));
    return;
  }
  if(mode==='bar'){
    let cnt=0;
    for(const r of barRoots)for(const c of r.children||[]){cols.appendChild(mkRow(c,0));cnt++;}
    if(CFG.so)for(const o of otherRoots)if(o.children&&o.children.length){cols.appendChild(mkRow(o,0));cnt++;}
    if(!cnt)cols.appendChild(el('div','cp-empty','书签栏为空（点上方「+ 书签」或右键新建）'));
  }else{
    const roots=(tree[0]&&tree[0].children)||[];let cnt=0;
    for(const r of roots){
      if(!r.children||!r.children.length)continue;
      cols.appendChild(el('div','cp-sep',(r.title||'书签')+(r.syncing?' (账号)':'')));
      for(const c of r.children){cols.appendChild(mkRow(c,0));cnt++;}
    }
    if(!cnt)cols.appendChild(el('div','cp-empty','暂无书签'));
  }
  for(let i=0;i<prev.length;i++){
    const fn=byId.get(prev[i].id);if(!fn||!isFld(fn))break;
    const ae=document.querySelector('.cp-item[data-id="'+CSS.escape(fn.id)+'"]');
    openFly(fn,i,ae?ae.getBoundingClientRect():prev[i].rc);
  }
};

const reload=async()=>{
  const t=await api(B.getTree);if(!t||!t[0])return;
  tree=t;byId=new Map();indexAll(t);
  const roots=t[0].children||[];
  barRoots=roots.filter(r=>r.folderType==='bookmarks-bar');
  if(!barRoots.length){const b1=roots.find(r=>r.id==='1')||roots[0];if(b1)barRoots=[b1];}
  barRoots.sort((a,b)=>(b.syncing?1:0)-(a.syncing?1:0));
  otherRoots=roots.filter(r=>r.folderType==='other');
  if(!otherRoots.length){const b2=roots.find(r=>r.id==='2');if(b2&&!barRoots.includes(b2))otherRoots=[b2];}
  const nb=barRoots.find(r=>r.children&&r.children.length>0);
  primaryBar=(nb||barRoots[0]||roots[0]||{id:'1'}).id;
  render();
};
const schedReload=()=>{clearTimeout(tReload);tReload=setTimeout(reload,40);};

on(inp,'input',()=>{query=inp.value;render();});
on(bBar,'click',()=>{mode='bar';bBar.classList.add('on');bAll.classList.remove('on');render();});
on(bAll,'click',()=>{mode='all';bAll.classList.add('on');bBar.classList.remove('on');render();});
on(bAddB,'click',()=>openEdit({m:'add-bm',pid:primaryBar}));
on(bAddF,'click',()=>openEdit({m:'add-fld',pid:primaryBar}));
on(main,'contextmenu',e=>{if(!e.target.closest||!e.target.closest('.cp-item')){e.preventDefault();showMenu(e.clientX,e.clientY,null,primaryBar);}});
on(document,'click',e=>{if(menu.classList.contains('show')&&!menu.contains(e.target))hideMenu();});
on(document,'keydown',e=>{
  if(e.key==='Escape'){
    if(modal)closeDlg();
    else if(menu.classList.contains('show'))hideMenu();
    else if(inp.value){inp.value='';query='';render();}
    else if(flyouts.length)closeFlyFrom(flyouts.length-1);
    else if(isPopup)signalHide();
  }else if((e.ctrlKey||e.metaKey)&&(e.key==='f'||e.key==='k')){
    e.preventDefault();inp.focus();inp.select();
  }
});

for(const ev of['onCreated','onRemoved','onChanged','onMoved','onChildrenReordered','onImportEnded']){
  if(B[ev]&&B[ev].addListener)B[ev].addListener(schedReload);
}
reload();
})();
)JS";

  std::string out;
  out.reserve(kPart1.size() + kPart2.size() + kPart3.size());
  out.append(kPart1);
  out.append(kPart2);
  out.append(kPart3);
  return out;
}

#endif  // CHROME_PLUS_SRC_BOOKMARKS_WEBUI_H_
