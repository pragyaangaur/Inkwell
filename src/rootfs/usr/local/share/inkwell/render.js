// render.js: the page renderer behind Books and arXiv. It runs under
// `mutool run` and keeps one document open, so turning a page doesn't
// reload the file. One command per line on stdin, fields split by tabs:
//   open <path> <w> <h> <em>   ok <pages> <reflowable> <title> <author>
//   layout <w> <h> <em>        ok <pages>        (reflowable documents only)
//   bounds <page>              ok <w> <h>        (in points)
//   render <page> <scale> <out.pgm>  ok <w> <h>  (grey, white background)
//   text <page>                ok <the first words on the page>
//   outline                    o <depth> <page> <title> per entry, then ok
// Anything that fails answers err and a message.
setUserCSS("body { margin: 0.4em 0 !important; } p { text-align: justify; hyphens: auto; }", true);

var doc = null;

function clean(s) { return String(s || "").replace(/[\t\r\n]+/g, " ").replace(/^\s+|\s+$/g, ""); }
function out(a) { print(a.join("\t")); }

function walk(items, depth, list) {
    for (var i = 0; i < items.length; i++) {
        var it = items[i], page = -1;
        try {
            var loc = doc.resolveLink(it.uri);
            page = typeof loc === "number" ? loc : doc.pageNumberFromLocation(loc);
        } catch (e) {}
        list.push(["o", depth, page, clean(it.title)]);
        if (it.down) walk(it.down, depth + 1, list);
    }
}

for (;;) {
    var line = readline();
    if (line === null || line === undefined) break;
    var f = String(line).split("\t");
    try {
        if (f[0] === "open") {
            doc = Document.openDocument(f[1]);
            var reflow = doc.isReflowable();
            if (reflow) doc.layout(+f[2], +f[3], +f[4]);
            out(["ok", doc.countPages(), reflow ? 1 : 0, clean(doc.getMetaData("info:Title")), clean(doc.getMetaData("info:Author"))]);
        } else if (f[0] === "layout") {
            doc.layout(+f[1], +f[2], +f[3]);
            out(["ok", doc.countPages()]);
        } else if (f[0] === "bounds") {
            var b = doc.loadPage(+f[1]).getBounds();
            out(["ok", b[2] - b[0], b[3] - b[1]]);
        } else if (f[0] === "render") {
            var s = +f[2];
            var pix = doc.loadPage(+f[1]).toPixmap(Matrix.scale(s, s), ColorSpace.DeviceGray, false);
            pix.saveAsPNM(f[3]);
            out(["ok", pix.getWidth(), pix.getHeight()]);
        } else if (f[0] === "text") {
            var t = clean(doc.loadPage(+f[1]).toStructuredText().asText());
            out(["ok", t.substring(0, 160)]);
        } else if (f[0] === "outline") {
            var list = [], o = doc.loadOutline();
            if (o) walk(o, 0, list);
            for (var i = 0; i < list.length && i < 400; i++) out(list[i]);
            out(["ok"]);
        } else {
            out(["err", "unknown command"]);
        }
    } catch (e) {
        out(["err", clean(e)]);
    }
}
