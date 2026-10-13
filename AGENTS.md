# DLL safety

Never use runtime `assert()` in DLL code paths, including debug builds. Log a
`STATUS:` failure and make the affected feature passive instead. Compile-time
`_Static_assert` checks are allowed. This rule does not apply to tests.
