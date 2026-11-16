# Root cause: The function that cannot be found

DC does not tell you that a function is private: from another module a private declaration is simply not there, so the diagnostic is `unknown path`. The message is accurate but easy to misread as a typo or a missing import.

The method for confusing diagnostics: gather evidence before changing code. The spelling matched, and `prices::with_tax` resolved, so the import and the module name were fine. The only difference between the working and the failing name was the word `public`.

When a name exists but cannot be found, check visibility, the import and the module name, in that order of cheapness.
