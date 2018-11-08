# Configurator

This is system that will allow a user to interactively 
configure the WiFi on their ESP32.

## Usage
The user (client) starts the device and connects to the
Configurator WiFi network. His/her device will receive
an IP address via DHCP. 

From there, the client connects to port 8080 on the default
gateway. They can issue a series of commands:

1. `list!`: List all the nearby APs
1. `scan!`: Rescan for nearby APs
1. `connect:x:y!`: Connect as a client to the `x` network
using password `y`.

After the client issues a connect command, their device
is disconnected from the network and the Configurator WiFi
network will disappear. Should the client specify incorrect
parameters for the connection, the Configurator network will
reappear and the client can try again.

## TODOs

Too many to list.
