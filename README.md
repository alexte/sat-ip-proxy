sat-ip-proxy
============

A proxy server for the SAT>IP protocol.

It has two parts:

- ssdp-server.js written in node.js. This server announces the sat>ip service to the local network and answers ssdp MSEARCH requests.

- rtsp-proxy written in C. This is an rtsp proxy specifically for the sat>ip service.

It works for me, but I tested it with inverto.tv Sat>IP Multibox (1.17) server and Elgato Sat>IP client (Android) only until now.

Based on the protocol specification: http://www.satip.info/sites/satip/files/resource/satip_specification_version_1_2_2.pdf

