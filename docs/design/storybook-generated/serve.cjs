// Static preview server: this directory only, on a free localhost port.
const http = require('node:http'), fs = require('node:fs'), path = require('node:path');
const root = __dirname;
const types = {'.html': 'text/html', '.js': 'text/javascript', '.png': 'image/png', '.json': 'application/json',
  '.ttf': 'font/ttf', '.md': 'text/plain'};

function reply(res, status) {
  res.writeHead(status);
  res.end();
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  let pathname;
  try {
    pathname = decodeURIComponent(url.pathname === '/' ? '/index.html' : url.pathname);
  } catch {
    return reply(res, 400); // a malformed escape such as /%E0%A4%A must not stop the server
  }
  const file = path.resolve(root, '.' + pathname);
  if (!file.startsWith(root + path.sep)) return reply(res, 403);
  fs.readFile(file, (error, data) => {
    if (error) return reply(res, 404);
    res.setHeader('Content-Type', types[path.extname(file)] || 'application/octet-stream');
    res.end(data);
  });
});
server.listen(0, '127.0.0.1', () => console.log(`Visual-only preview: http://127.0.0.1:${server.address().port}/`));
