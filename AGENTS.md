# C++ Project Rules & Guidelines

This file is an **index only**. Normative requirements live in documents 01–04 under [`rules/`](rules/); document 05 contains non-normative examples.
User- and maintainer-facing explanations, build commands, and current tool/CI enforcement live under [`docs/`](docs/).

Across the rules, **must / must not** are hard requirements, **prefer / normally** describe defaults, and **may** permits an exception under the stated conditions. Tool configuration and CI checks support these rules but do not prove all requirements are automatically enforced.

| # | Document | Topics |
|---|----------|--------|
| 01 | [Language, Compiler & Build](rules/01-language-and-build.md) | Language and build, Dependencies, Build quality and tooling |
| 02 | [Implementation](rules/02-implementation.md) | Scope & Architecture Boundaries, Reuse, Headers, Ownership & ABI, Interfaces, Style, Strings, Templates & Lambdas |
| 03 | [Error Handling](rules/03-error-handling.md) | Core Principles, Contracts & Assertions, Mechanism Selection, Exception Safety & noexcept, Catch Boundaries, Deep Call Chain Translation |
| 04 | [Quality & Process](rules/04-quality-and-process.md) | Concurrency, Security, Testing, Logging, Documentation, Performance, Cross-Platform, VCS, Code Review |
| 05 | [Examples (Non-normative)](rules/05-examples.md) | Checked Results, RAII, C-string Adaptation, Boundary Examples |
