# C++ Project Rules & Guidelines

## Mandatory Reading

**Before writing, modifying, or reviewing any C++ code in this repository, you must read all five documents listed below in full** — not only the ones that look topically relevant. They cross-reference each other, so rules outside the apparent topic still apply.

This applies to every task that touches source files, build files, or tests, including small edits. It does not apply to purely conversational questions that require no code change.

Documents 01–04 are **normative**. Document 05 is **non-normative** examples, but is still required reading because it shows how the normative rules are meant to be applied in practice.

If a rule is genuinely unclear or conflicts with the task at hand, say so explicitly rather than silently choosing an interpretation.

| # | Document | Topics |
|---|----------|--------|
| 01 | [Language, Compiler & Build](rules/01-language-and-build.md) | Language and build, Dependencies, Build quality and tooling |
| 02 | [Implementation](rules/02-implementation.md) | Scope & Architecture Boundaries, Reuse, Headers, Ownership & ABI, Interfaces, Style, Strings, Templates & Lambdas |
| 03 | [Error Handling](rules/03-error-handling.md) | Core Principles, Contracts & Assertions, Mechanism Selection, Exception Safety & noexcept, Catch Boundaries, Deep Call Chain Translation |
| 04 | [Quality & Process](rules/04-quality-and-process.md) | Concurrency, Security, Testing, Logging, Documentation, Performance, Cross-Platform, VCS, Code Review |
| 05 | [Examples (Non-normative)](rules/05-examples.md) | Checked Results, RAII, C-string Adaptation, Boundary Examples |

Across the rules, **must / must not** are hard requirements, **prefer / normally** describe defaults, and **may** permits an exception under the stated conditions. Tool configuration and CI checks support these rules but do not prove all requirements are automatically enforced.

## Reference

User- and maintainer-facing explanations, build commands, and current tool/CI enforcement live under [`docs/`](docs/). These are informative, not normative.
