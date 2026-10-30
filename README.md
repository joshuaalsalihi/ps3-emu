# ps3-emu

Seeing if giving Anthropic $20/month is enough to fulfill a childhood dream.

An accuracy-focused PlayStation 3 emulator, built for research, homebrew development and learning
how the Cell processor works.

**Status:** design phase. See the [spec](docs/SPEC.md) for the architecture and roadmap.

## Goals

- **Exact:** every CPU instruction behaves like the hardware, flags and edge cases included.
- **Deterministic:** the same inputs give the same output, so runs can be replayed and compared.
- **Inspectable:** tracing, logging and GDB debugging are built in.
- **Readable:** the code is written to be understood.

Running commercial games at full speed is not an early goal.

## Legal

This is a clean-room implementation, based only on public documentation. It contains no Sony
firmware, keys or code. You must supply your own firmware and software.

## License

[0BSD](LICENSE)
