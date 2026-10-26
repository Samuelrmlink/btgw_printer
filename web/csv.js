/** CSV / TSV / pasted-list parsing for batch printing. DOM-free.
 *
 * RFC 4180 quoting ("a ""b""", embedded newlines), CRLF or LF, a BOM, and the
 * delimiter guessed from the first line: tab, then semicolon, then comma.
 * A single column of lines (a pasted list) parses as one-field rows. */

export function guessDelimiter(text) {
    const first = String(text).split(/\r?\n/, 1)[0] || "";
    let inQ = false;
    const count = { "\t": 0, ";": 0, ",": 0 };
    for (const c of first) {
        if (c === '"') {
            inQ = !inQ;
        } else if (!inQ && c in count) {
            count[c]++;
        }
    }
    if (count["\t"]) {
        return "\t";
    }
    if (count[";"] > count[","]) {
        return ";";
    }
    return ",";
}

export function parseCsv(text, delim) {
    let s = String(text || "");
    if (s.charCodeAt(0) === 0xfeff) {
        s = s.slice(1);
    }
    const d = delim || guessDelimiter(s);
    const rows = [];
    let row = [];
    let field = "";
    let inQ = false;
    let i = 0;
    let any = false;
    while (i < s.length) {
        const c = s[i];
        if (inQ) {
            if (c === '"') {
                if (s[i + 1] === '"') {
                    field += '"';
                    i += 2;
                    continue;
                }
                inQ = false;
                i++;
                continue;
            }
            field += c;
            i++;
            continue;
        }
        if (c === '"' && field === "") {
            inQ = true;
            any = true;
            i++;
            continue;
        }
        if (c === d) {
            row.push(field);
            field = "";
            any = true;
            i++;
            continue;
        }
        if (c === "\r" || c === "\n") {
            row.push(field);
            rows.push(row);
            row = [];
            field = "";
            any = false;
            if (c === "\r" && s[i + 1] === "\n") {
                i++;
            }
            i++;
            continue;
        }
        field += c;
        any = true;
        i++;
    }
    if (any || field !== "" || row.length) {
        row.push(field);
        rows.push(row);
    }
    // Drop blank lines (a row of one empty field).
    return rows.filter((r) => !(r.length === 1 && r[0].trim() === ""));
}

/** {header, rows}. With hasHeader false the header is col1..colN. */
export function parseTable(text, { hasHeader = true, delim } = {}) {
    const all = parseCsv(text, delim);
    if (!all.length) {
        return { header: [], rows: [] };
    }
    const width = Math.max(...all.map((r) => r.length));
    const padRow = (r) => (r.length < width ? r.concat(Array(width - r.length).fill("")) : r);
    if (hasHeader) {
        return { header: padRow(all[0]).map((h, i) => h.trim() || `col${i + 1}`), rows: all.slice(1).map(padRow) };
    }
    return { header: Array.from({ length: width }, (_, i) => `col${i + 1}`), rows: all.map(padRow) };
}
