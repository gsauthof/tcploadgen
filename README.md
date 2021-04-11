This repository contains tcploadgen - a TCP client for driving
some sessions against a simple TLV-PDU style protocol server.

2021, Georg Sauthoff <mail@gms.tf>


## Configuration

The client is highly configurable via a [TOML][toml] file (cf. the
`flow.toml` example). That means number of sessions, session
parameters, number of sender threads, thread-to-core mapping,
protocol PDU details, timings etc. are all defined in a TOML
configuration file.

## Supported Protocols

The client supports protocols that are a sequence of
self-delimiting TLV (Tag Length Value) PDUs (protocol data
units) and have some concept of a session and error messages.

Basically, supporting a new protocol just requires defining the
offsets/sizes of the tag and length fields. And which tag
indicates an error (and at which offset the length/text of an
error message is stored).

## Timings

Sessions can be distributed over multiple sender threads. Each
sender thread uses high-resolution timers to trigger message
transmissions. In combination with a PTP synchronized clock this
allows for - so to say - more deterministic load patterns.

## Payload and Templates

The login/session setup flow (prelude) and the main session flow
can be parametrized by a set of variables and operations (such as
incrementing a message sequence number). Declaring a variable is
basically just a matter of specifying its offset and size. The
type is derived from the TOML value of the initial assignment.

A flow is basically a list of packet payloads (i.e. the PDUs)
where each payload (a.k.a. `pkt`) is specified as hex-string.
Variables and actions are applied on payloads, where specified.


## See also

An example for such a TLV protocol is ETI (or rather it's LTV if you
will). See [python-eti](https://github.com/gsauthof/python-eti)
for details and bindings that can be used to generate payload
hex-strings.

Example:

    from eti.v9_0 import *
    x = LogonRequest()
    x.ApplicationSystemVendor = b'...'
    # etc. overwrite some other default values
    x.pack().hex()


## Dependencies

- C++17
- cmake
- [libixxx](https://github.com/gsauthof/libixxx)
- [libixxxutil](https://github.com/gsauthof/libixxxutil)
- [tomlplusplus](https://github.com/marzer/tomlplusplus)

Where these libraries are referenced via git submodules.

## Build Instructions

You basically just have to make sure that the submodules are
checked out. Otherwise it's just the usual cmake dance. For
example:

```
git clone https://github.com/gsauthof/tcploadgen.git
git submodule update --init
mkdir build
cd build
CXXFLAGS='-O3 -g -Wall' cmake ..
make
```

Or if Ninja is available you can replace the last commands with something
like:

```
CXXFLAGS='-O3 -g -Wall' cmake -G Ninja ..
ninja
```

[toml]: https://toml.io/en/v1.0.0#spec

