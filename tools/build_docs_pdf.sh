#!/usr/bin/env bash
# Renders docs/*.md to docs/pdf/*.pdf (one per document) plus a combined
# docs/pdf/myOS-documentacion.pdf. Requires: pandoc, weasyprint.
set -euo pipefail
cd "$(dirname "$0")/../docs"
CSS="../tools/docs.css"
OUT="pdf"
mkdir -p "$OUT"

DOCS=(01-vision-y-estrategia 02-arquitectura 03-modulos 04-build-y-tests 05-metodologia-multiagente 06-estado-y-avance)

render() {  # render <html-in> <pdf-out>
    weasyprint -s "$CSS" --base-url "$PWD/" "$1" "$2" 2>/dev/null
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for d in README "${DOCS[@]}"; do
    pandoc "$d.md" -f gfm -t html5 --standalone --metadata title="myOS — $d" \
        -V title= -o "$tmp/$d.html"
    render "$tmp/$d.html" "$OUT/$d.pdf"
    echo "  $OUT/$d.pdf"
done

# Combined document: cover + index + every chapter starting on a new page.
{
    echo '<div class="cover"><h1>myOS</h1>'
    echo '<p>Documentación técnica del proyecto</p>'
    echo "<p>Sistema operativo x86_64 escrito desde cero · $(date '+%d/%m/%Y')</p>"
    echo '<p><img src="img/desktop-v0.1-macos.png" style="width:75%;margin-top:18mm"></p></div>'
    for d in "${DOCS[@]}"; do
        pandoc "$d.md" -f gfm -t html5 | sed '0,/<h1/s//<h1 class="doc-break"/'
    done
} > "$tmp/body.html"
pandoc "$tmp/body.html" -f html -t html5 --standalone --metadata title="myOS — Documentación" \
    -V title= -o "$tmp/all.html"
render "$tmp/all.html" "$OUT/myOS-documentacion.pdf"
echo "  $OUT/myOS-documentacion.pdf"
