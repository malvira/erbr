Erbium Border Router
====================

This is the same as `examples/ipv6/border-router` except that Eribum
is used instead of httpd-simple.

`rplinfo` resources are activated by default. See
https://github.com/malvira/rplinfo for more information about
`rplinfo`

Setup and Building
==================

``` 
git clone https://github.com/malvira/erbr.git
cd erbr
git submodule update --init
make TARGET=econotag
```



