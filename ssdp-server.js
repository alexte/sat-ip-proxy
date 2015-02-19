/*
 *  alexte/sat-ip-proxy
 *
 *  ssdp listener, because most sat>ip clients are missing manual configuration of
 *  sat>ip server.
 *
 */

var os = require('os');
var http = require('http');
var fs = require('fs');
var SSDP = require('node-ssdp').Server;

// ------------------------ configurable paramters
var listen="guess";
var http_port=8081;
var debug=1;

// the following should match your your sat>ip server "DVBx-n" 
//      (x: S,S2,T,T2,C,..; n: number of tuners)
var SATIPCAP="DVBS2-4";    
 
if (listen=="guess")
{
    // try to guess en internal interface and its ip address
    var ifaces=os.networkInterfaces();
    var iface;

    for (var ifaceId in ifaces)
    {
	iface=ifaces[ifaceId];
	if (iface[0].address.indexOf("192.168.")==0) break;
	if (iface[0].address.indexOf("10.")==0) break;
	if (iface[0].address.indexOf("172.16.")==0) break;
	if (iface[0].address.indexOf("172.17.")==0) break;
	if (iface[0].address.indexOf("172.18.")==0) break;
	if (iface[0].address.indexOf("172.19.")==0) break;
    }
    listen=iface[0].address;
}

console.log("Starting to listening on "+listen+" ("+http_port+")");

// ------------------- ssdp server
var server = new SSDP({
    // logLevel: 'TRACE',
    // unicastHost: '192.168.0.1',
    location: 'http://'+listen+':'+http_port+'/desc.xml'
  })

server.addUSN('upnp:rootdevice');
server.addUSN('urn:ses-com:device:SatIPServer:1');

server.start(listen);

// -------------------- http server for desc and logos
var descfile = fs.readFileSync('desc.xml').toString();
descfile=descfile.replace("$SATIPCAP$",SATIPCAP);

http.createServer(function (req, res) {
  if (debug>0) console.log("HTTP Request: "+req.connection.remoteAddress+" "+req.url);
  if (req.url=="/desc.xml")
  {
      res.writeHead(200, {'Content-Type': 'application/xml'});
      res.end(descfile);
  }
  else
  {
      res.writeHead(404, {'Content-Type': 'text/plain'});
      res.end("File not found!");
  }
}).listen(http_port,listen);

