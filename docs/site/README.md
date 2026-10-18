# The documentation site

The documentation is written as markdown that reads on GitHub, with
relative links between the files as they lie in the repository. This
directory turns the same files into a static site for GitHub Pages, with
pandoc, keeping the repository's layout so that every link still works:
`README.md` becomes `index.html`, `docs/tutorial.md` becomes
`docs/tutorial.html`, `samples/README.md` becomes `samples/index.html`,
and each sample program gets a highlighted page of its own.

```bash
python docs/site/build.py --out build/site --check
```

needs pandoc 2.11 or later on PATH, and the playground's module built
first for the playground to be part of the site. The pages land in `build/site`; open
`build/site/index.html` in a browser. `--check` verifies every internal
link and fragment of the built site, including the `spec §` links between
documents, and fails on a broken one.

| File | Role |
|---|---|
| `build.py` | lists the pages, runs pandoc on each, copies the logo and the extension, checks the links |
| `template.html` | the page: a top bar, a sidebar table of contents, the text |
| `style.css` | the one stylesheet |
| `links.lua` | a pandoc filter: `.md` links become `.html`, `.goose` links reach the sample pages, and the `<details>` table of contents each file carries for GitHub is dropped in favour of the sidebar |
| `goose.xml` | Goose syntax highlighting for pandoc, in the KDE syntax definition format |
| `play/` | the playground: `index.html`, `play.mjs` and `play.css` are the page, `worker.mjs` the worker a run of `goose.wasm` happens in, `wasi.mjs` the WASI shim it runs on (`docs/design/interpreter.md` §9), `wasi_test.mjs` the test of the shim and the worker under node |

The playground needs the compiler built to WebAssembly: `--wasm` names the
module (default `build/wasm/goose.wasm`, where `test/wasm_build.py` puts
it); the build copies it into `play/` with `files.json`, the standard
library and the text samples the page's file tree starts with, and links
the page from every page's navigation. Without the module the playground
is left out and the build says so.

Pandoc reads the files as GitHub-flavored markdown, so heading anchors
are the ones GitHub makes (`#33-the-array-family`), and the cross-document
links in the tutorial and the reference resolve on both.

The workflow `.github/workflows/pages.yml` builds the site on every push
to `master` and deploys it with GitHub's Pages actions. For that to work,
the repository's Pages setting (Settings, Pages, Build and deployment)
must have its source set to **GitHub Actions**; the workflow does not
change that setting itself.

To add a page, write the markdown where it belongs in the repository and
add it to `PAGES` in `build.py` if its directory is not already listed
(every `docs/*.md`, `docs/design/*.md` and `bench/*.md` is picked up).
The site's own index is [`docs/index.md`](../index.md).
