/*
 *  alexte/sat-ip-proxy
 *
 *  ssdp listener, because most sat>ip clients are missing manual configuration of
 *  sat>ip server.
 *
 */

var os = require('os');
var SSDP = require('node-ssdp').Server;

var listen=false;
 
if (!listen)
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

console.log("Starting to listening on "+listen);

var server = new SSDP({
    // logLevel: 'TRACE',
    // unicastHost: '192.168.0.1',
    location: 'http://'+listen+'/desc.xml'
  })

server.addUSN('upnp:rootdevice');
server.addUSN('urn:ses-com:device:SatIPServer:1');

server.start(listen);

