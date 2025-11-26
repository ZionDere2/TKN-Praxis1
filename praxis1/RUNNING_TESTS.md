# Running the Praxis tests locally

The course test runner lives under the `test/` directory. Always execute it from the
`praxis1` project root (the directory containing `CMakeLists.txt`). From that
directory, run for example:

```bash
bash test/check_submission.sh praxis1           # full suite
bash test/check_submission.sh praxis1 -k test_listen  # specific tests
bash test/check_submission.sh praxis1 --reinstall     # rebuild venv, then run
```

If your shell cannot find `test/check_submission.sh`, change to the project root
(`cd /path/to/TKN-Praxis1/praxis1`) before launching the command.

> The script packages the project, rebuilds it in a temporary directory, and calls
> pytest with the correct executable paths. You do not need to start the server
> manually.
