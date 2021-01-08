sat-ip-proxy
============

A proxy server for the SAT>IP protocol.

The SAT>IP spezification focuses mainly on home use. Automatic service discovery is mandatory.
Because of this, most SAT>IP clients don't allow manual SAT>IP server configuration. Only automatic 
server discovery is supported. I would say they should call it SAT>LAN because these clients
don't support the core concepts of the Inter Net protocol.

This proxy is a simple workaround to circumvent the limitations of LAN-only clients.
The proxy has to run in the client network and can connect to any SAT>IP server which is
reachable by IP.

It has two parts:

- ssdp-server.js written in node.js. This server announces the sat>ip service to the local network and answers ssdp MSEARCH requests.

- rtsp-proxy written in C. This is an rtsp proxy specifically for the sat>ip service.

It works for me, but I only tested it with this server:

- inverto.tv Sat>IP Multibox (1.17)

and these clients:

- Elgato Sat>IP client (Android)
- Kathrein Sat Receiver UFSConnect 906

The software is based on the protocol specification: http://www.satip.info/sites/satip/files/resource/satip_specification_version_1_2_2.pdf

Usage scenarios
---------------

If you have more then one network. E.g. you have a router that seperates your WLAN from your server LAN,
you can put this proxy on any computer in your WLAN network or the router. This enables SAT>IP for your WLAN 
clients (e.g. Elgato SAT>IP Client).

You may also use a remote SAT>IP server, if the SAT>IP server is reachable by IP.

Futhermore you can use it as a SAT>IP RTSP protocol debugger interpossing it the middle.

Dependencies
------------

You need *gcc* and *make* to compile the rtsp proxy.
You need node.js and npm to run the ssdp server.

Compile
-------

```
npm install
make
```

Sample startup
--------------

If 192.168.42.40 is your SAT>IP server:

```
node ssdp-server.js &
./rtsp-proxy  192.168.42.40
```

Daemon mode is not implemented yet. You may start the server in background or use "screen"

```
screen -d -m sh -c "node ssdp-server.js & ./rtsp-proxy  192.168.42.40"
```

Security
--------

This software is not developed with security in mind. It will probably break when a client or server 
misbehaves. So please use this only in safe environments.
