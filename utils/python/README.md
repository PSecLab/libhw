# Python bindings for libhw

ctypes bindings over `libhw.so`. No third-party dependency, and nothing to
compile in the bindings themselves.

## Installing

```bash
pip install ./utils/python          # from a libhw checkout
```

The wheel carries its own copy of `libhw.so`, so an installed package works
from anywhere with no repository in sight. If the library has not been built
yet, the build runs `make` for you; that needs a compiler and the libstlink
development headers (`libstlink-dev` on Debian/Ubuntu).

For development, an editable install keeps using the library you are building:

```bash
pip install -e ./utils/python
make                                 # rebuild; the editable install picks it up
```

The search order is `LIBHW_LIBRARY`, then a build tree above the package, then
the copy bundled in the wheel, then the system. A build tree comes before the
bundled copy on purpose: in an editable install both exist, and the one you
just built is the one you meant.

Without installing at all:

```bash
make && export PYTHONPATH=$PWD/utils/python
```

The sdist is not self-contained. The C sources live in the repository above
this directory, and copying them in would mean maintaining a second copy, so
building needs the checkout. That is how this package is used in practice; it
is not published to PyPI.

## Command line

```bash
libhw-probe                # identify the mock target and report its state
libhw-probe stlink         # ... a real board
libhw-probe stlink --verbose --resume
```

## Connecting

`Hw` is a context manager, and every failing call raises rather than returning
a status code.

```python
from libhw import connect

with connect("mock") as hw:          # or "stlink", "openocd"
    hw.write32(0x20000000, 0xDEADBEEF)
    print(hex(hw.read32(0x20000000)))

    hw.halt()
    print("halted:", hw.halted)
    print("PC:", hex(hw.read_reg(15)))
    hw.run()
```

## Flash

`hw.flash` mirrors the C API, including the part that catches people out:
**`write` does not erase**, so on its own it can only clear bits.

```python
info = hw.flash.info()
# FlashInfo(base=0x08000000, size=262144, page_size=1024, write_align=4)

hw.flash.update(info.base, image)         # erase, reprogram, verify
data = hw.flash.read(info.base, 256)
base, size = hw.flash.sector(addr)        # the erase block containing addr
```

`patch` applies only what changed. Blocks identical in both images are never
touched, and with `old` supplied they are never even read. A changed block is
erased only if some bit has to go 0 → 1.

```python
stats = hw.flash.patch(info.base, old_image, new_image)
print(stats.sectors_changed, "of", stats.sectors_total, "sectors changed")
print(stats.sectors_erased, "erased,", stats.bytes_written, "bytes written")
```

Pass `old=None` to read the current contents instead — always correct, but it
costs a full read of the region.

## Architectural state

The generated state database is exposed as ordinary Python objects.

```python
from libhw import state_db, Presence, ReadStatus

db = state_db("armv7m")                   # or a CPU overlay
print(db.source)                          # 'DDI0403 E.e'
print(len(db), "state elements")

primask = db.find("PRIMASK")
print(primask.encoding, primask.namespace, primask.width)

for alias in db.aliases:
    print(alias.name, "->", alias.target)
```

### Implemented is not readable

These are separate, and the API keeps them separate. A clean read is **not**
evidence that a register exists: an unimplemented word in the Private
Peripheral Bus generally reads as zero rather than faulting.

```python
features = hw.identify()
result = hw.query(db.find("MPU_TYPE"), features)

result.presence        # Presence.CONFIG_DENIED  -- from MPU_TYPE.DREGION
result.evidence        # 'MPU'                   -- what decided it
result.read            # ReadStatus.NOT_ATTEMPTED
result.is_implemented  # False, and never inferred from result.read
```

`Presence.is_implemented` is true only on positive evidence: `ARCHITECTURAL`,
`CONFIG_CONFIRMED` or `DISCOVERED`. `UNKNOWN` is not implemented.

`hw.snapshot()` reads every snapshot-marked element the target actually
implements, skipping anything with no presence evidence behind it.

## Two encodings for the same register

A register reachable both by instruction and by debugger has two different
encodings, and conflating them reads the wrong thing:

```python
db.find("MSP").encoding      # 'SYSm=8'   -- the MRS/MSR operand
db.debug_reg("MSP").regsel   # 17         -- the DCRSR selector
```

## Examples

```bash
python3 examples/flash_patch.py          # differential flash update
python3 examples/state_dump.py           # identify a target, report its state
python3 examples/state_dump.py stlink    # ... against a real board
```

## Tests

```bash
python3 -m pytest tests -q          # or, from the repository root:
make python-check                   # tests against the source tree
make python-install-check           # build a wheel, install it, test that
```

Everything runs against the `mock` backend, so no board is needed.

The first test is the one that matters: ctypes struct definitions that drift
from the C headers do not fail loudly, they silently read the wrong bytes. So
the suite compiles a probe that reports the real `sizeof`/`offsetof` of every
bound struct and compares it against the ctypes layout, rather than trusting
that the two stayed in step. It skips if no C compiler is present.
