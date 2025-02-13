# Fiber::Profiler

Detects fibers that are stalling the event loop.

[![Development Status](https://github.com/socketry/fiber-profiler/workflows/Test/badge.svg)](https://github.com/socketry/fiber-profiler/actions?workflow=Test)

## Motivation

Migrating existing applications to the event loop can be tricky. One of the most common issues is when a fiber is blocking the event loop. This can happen when a fiber is waiting on a blocking operation, such as a database query, or a network request, that could not take advantage of the event loop. This can cause the event loop to stall, and prevent other fibers from running. This gem provides a way to detect these fibers, and help you identify the cause of the stall.

## Usage

Please see the [project documentation](https://socketry.github.io/fiber-profiler/) for more details.

  - [Getting Started](https://socketry.github.io/fiber-profiler/guides/getting-started/index) - This guide explains how to detect stalls using the fiber profiler.

## Releases

Please see the [project releases](https://socketry.github.io/fiber-profiler/releases/index) for all releases.

### v0.1.0

  - Initial implementation extracted from `io-event` gem.

## Contributing

We welcome contributions to this project.

1.  Fork it.
2.  Create your feature branch (`git checkout -b my-new-feature`).
3.  Commit your changes (`git commit -am 'Add some feature'`).
4.  Push to the branch (`git push origin my-new-feature`).
5.  Create new Pull Request.

### Developer Certificate of Origin

In order to protect users of this project, we require all contributors to comply with the [Developer Certificate of Origin](https://developercertificate.org/). This ensures that all contributions are properly licensed and attributed.

### Community Guidelines

This project is best served by a collaborative and respectful environment. Treat each other professionally, respect differing viewpoints, and engage constructively. Harassment, discrimination, or harmful behavior is not tolerated. Communicate clearly, listen actively, and support one another. If any issues arise, please inform the project maintainers.
