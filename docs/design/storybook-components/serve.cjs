const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const server=http.createServer((req,res)=>{
  const url=new URL(req.url,'http://127.0.0.1');
  const file=path.resolve(root,'.'+decodeURIComponent(url.pathname));
  if(!file.startsWith(root+path.sep)){res.writeHead(403);res.end();return;}
  fs.readFile(file,(error,data)=>{if(error){res.writeHead(404);res.end();return;}res.setHeader('Content-Type',({'.html':'text/html','.png':'image/png','.json':'application/json','.cjs':'text/plain','.md':'text/plain'})[path.extname(file)]||'application/octet-stream');res.end(data);});
}).listen(0,'127.0.0.1',()=>console.log(`Visual-only proof: http://127.0.0.1:${server.address().port}/storybook-components/index.html`));
