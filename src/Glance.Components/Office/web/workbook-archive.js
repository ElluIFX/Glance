import JSZip from 'jszip';
import { SaxesParser } from 'saxes';

const spreadsheetNamespace = 'http://schemas.openxmlformats.org/spreadsheetml/2006/main';
const relationshipNamespace = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships';
const packageNamespace = 'http://schemas.openxmlformats.org/package/2006/relationships';
const drawingNamespace = 'http://schemas.openxmlformats.org/drawingml/2006/main';

export function workbookThemeColors(xml) {
    const colors = ['FFFFFF', '000000'];
    if (!xml) return colors;
    const order = ['lt1', 'dk1', 'lt2', 'dk2', 'accent1', 'accent2', 'accent3', 'accent4', 'accent5', 'accent6', 'hlink', 'folHlink'];
    const stack = [];
    const sax = parser();
    sax.on('opentag', tag => {
        const parent = stack.at(-1), grandparent = stack.at(-2);
        if (tag.uri === drawingNamespace && parent?.uri === drawingNamespace &&
            grandparent?.uri === drawingNamespace && grandparent.local === 'clrScheme') {
            const index = order.indexOf(parent.local);
            const value = attribute(tag, tag.local === 'sysClr' ? 'lastClr' : 'val');
            if (index >= 0 && ['srgbClr', 'sysClr'].includes(tag.local) && /^[0-9a-f]{6}$/i.test(value ?? '')) colors[index] = value;
        }
        stack.push(tag);
    });
    sax.on('closetag', () => stack.pop());
    sax.write(xml).close();
    return colors;
}

function attribute(tag, name, uri = '') {
    if (!uri) {
        const value = tag.attributes[name];
        return value?.uri === '' ? value.value : undefined;
    }
    return Object.values(tag.attributes).find(value => value.local === name && value.uri === uri)?.value;
}

function parser() {
    const result = new SaxesParser({ xmlns: true });
    result.on('doctype', () => { throw new Error('Document types are not supported'); });
    return result;
}

function escapeXml(value) {
    return value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;').replaceAll('\r', '&#13;').replaceAll('\n', '&#10;').replaceAll('\t', '&#9;');
}

function columnNumber(text) {
    return [...text].reduce((value, char) => value * 26 + char.charCodeAt(0) - 64, 0);
}

export function projectWorksheetRows(xml, rowLimit = 60) {
    if (!Number.isSafeInteger(rowLimit) || rowLimit < 1 || rowLimit > 1048576) throw new RangeError('Invalid row limit');
    const sax = parser();
    const output = [];
    let depth = 0, skipped = 0, dataDepth = -1, row = 0, rows = 0, cells = 0;
    let foundData = false;
    let mergedCells = 0;
    let columns = 0, cellColumn = 0;
    const rowSettings = [];
    const drawingIds = [];
    sax.on('opentag', tag => {
        ++depth;
        if (tag.uri === spreadsheetNamespace && tag.local === 'drawing') {
            const id = attribute(tag, 'id', relationshipNamespace);
            if (!id || drawingIds.includes(id)) throw new Error('Invalid worksheet drawing');
            drawingIds.push(id);
        }
        if (tag.uri === spreadsheetNamespace && tag.local === 'row') cellColumn = 0;
        if (tag.uri === spreadsheetNamespace && tag.local === 'c') {
            if (++cells > 1000000) throw new Error('Worksheet exceeds cell budget');
            const reference = attribute(tag, 'r');
            const match = reference?.match(/^([A-Z]+)[1-9][0-9]*$/);
            if (reference && !match) throw new Error('Invalid cell reference');
            cellColumn = match ? columnNumber(match[1]) : cellColumn + 1;
            if (cellColumn > 16384) throw new Error('Invalid worksheet column');
            columns = Math.max(columns, cellColumn);
        }
        if (skipped) { ++skipped; return; }
        if (depth === 2 && tag.uri === spreadsheetNamespace && tag.local === 'sheetData') {
            if (foundData) throw new Error('Duplicate worksheet data');
            foundData = true;
            dataDepth = depth;
        }
        if (depth === dataDepth + 1 && tag.uri === spreadsheetNamespace && tag.local === 'row') {
            row = Number(attribute(tag, 'r') ?? row + 1);
            if (!Number.isSafeInteger(row) || row < 1 || row > 1048576) throw new Error('Invalid worksheet row');
            rows = Math.max(rows, row);
            const height = attribute(tag, 'ht');
            const hidden = ['1', 'true'].includes(attribute(tag, 'hidden'));
            if (height !== undefined && (!Number.isFinite(Number(height)) || Number(height) < 0)) throw new Error('Invalid row height');
            if (height !== undefined || hidden) rowSettings.push({ index: row, height: height === undefined ? undefined : Number(height), hidden });
            if (row > rowLimit) { skipped = 1; return; }
        }
        let mergeReference;
        if (tag.uri === spreadsheetNamespace && tag.local === 'mergeCell') {
            const reference = attribute(tag, 'ref')?.match(/^([A-Z]+)([1-9][0-9]*):([A-Z]+)([1-9][0-9]*)$/);
            if (!reference) throw new Error('Invalid merged cell reference');
            const left = columnNumber(reference[1]), right = columnNumber(reference[3]);
            const top = Number(reference[2]), bottom = Number(reference[4]);
            if (left > right || right > 16384 || top > bottom || bottom > 1048576) throw new Error('Invalid merged cell bounds');
            mergedCells += (right - left + 1) * (bottom - top + 1);
            if (mergedCells > 1000000) throw new Error('Worksheet exceeds merged cell budget');
            rows = Math.max(rows, bottom);
            columns = Math.max(columns, right);
            if (Number(reference[2]) > rowLimit) { skipped = 1; return; }
            mergeReference = `${reference[1]}${reference[2]}:${reference[3]}${Math.min(Number(reference[4]), rowLimit)}`;
        }
        output.push('<', tag.name);
        for (const value of Object.values(tag.attributes)) {
            const text = mergeReference && value.local === 'ref' && !value.uri ? mergeReference : value.value;
            output.push(' ', value.name, '="', escapeXml(text), '"');
        }
        output.push('>');
    });
    sax.on('closetag', tag => {
        if (skipped) --skipped;
        else output.push('</', tag.name, '>');
        if (depth === dataDepth) dataDepth = -1;
        --depth;
    });
    sax.on('text', value => { if (!skipped && depth > 0) output.push(escapeXml(value)); });
    sax.on('cdata', value => { if (!skipped) output.push(escapeXml(value)); });
    sax.write(xml).close();
    if (!foundData) throw new Error('Worksheet data is missing');
    return { xml: output.join(''), rows, columns, rowSettings, drawingIds, truncated: rows > rowLimit };
}

export function resolveWorkbookPart(base, target) {
    target = target && decodeURIComponent(target);
    if (!target || target.includes('\\') || target.includes(':') || target.includes('?') || target.includes('#')) {
        throw new Error('Invalid worksheet target');
    }
    const path = target.startsWith('/') ? [] : base.split('/').slice(0, -1);
    for (const part of target.split('/')) {
        if (!part || part === '.') continue;
        if (part === '..') {
            if (!path.length) throw new Error('Worksheet target escapes package');
            path.pop();
        } else path.push(part);
    }
    return path.join('/');
}

export async function openWorkbookArchive(bytes, signal) {
    signal?.throwIfAborted();
    if (bytes.byteLength > 128 * 1024 * 1024) throw new Error('Workbook exceeds preview budget');
    const zip = await JSZip.loadAsync(bytes);
    const entries = Object.values(zip.files);
    if (entries.length > 20000) throw new Error('Workbook has too many parts');
    let total = 0;
    for (const entry of entries) {
        const size = entry._data?.uncompressedSize ?? 0;
        total += size;
        if (size > 64 * 1024 * 1024 || total > 512 * 1024 * 1024) throw new Error('Workbook exceeds unpacked budget');
    }
    const book = zip.file('xl/workbook.xml');
    const links = zip.file('xl/_rels/workbook.xml.rels');
    if (!book || !links) throw new Error('Workbook parts are missing');
    const relations = new Map();
    const linkParser = parser();
    linkParser.on('opentag', tag => {
        if (tag.uri !== packageNamespace || tag.local !== 'Relationship') return;
        if (attribute(tag, 'Type') !== `${relationshipNamespace}/worksheet`) return;
        if (attribute(tag, 'TargetMode') === 'External') throw new Error('External worksheets are unsupported');
        const id = attribute(tag, 'Id');
        if (!id || relations.has(id)) throw new Error('Duplicate worksheet relationship');
        relations.set(id, resolveWorkbookPart('xl/workbook.xml', attribute(tag, 'Target')));
    });
    linkParser.write(await links.async('string')).close();
    const sheets = [];
    let activeTab = 0;
    const bookParser = parser();
    bookParser.on('opentag', tag => {
        if (tag.uri !== spreadsheetNamespace) return;
        if (tag.local === 'workbookView') activeTab = Number(attribute(tag, 'activeTab') ?? 0);
        if (tag.local !== 'sheet') return;
        const target = relations.get(attribute(tag, 'id', relationshipNamespace));
        const name = attribute(tag, 'name');
        if (!target || !zip.file(target) || !name || sheets.some(sheet => sheet.name === name || sheet.target === target)) {
            throw new Error('Invalid worksheet registration');
        }
        sheets.push({ name, target, hidden: ['hidden', 'veryHidden'].includes(attribute(tag, 'state')) });
    });
    bookParser.write(await book.async('string')).close();
    if (!sheets[activeTab] || sheets[activeTab].hidden) activeTab = sheets.findIndex(sheet => !sheet.hidden);
    if (activeTab < 0 || sheets.length === 0) throw new Error('No visible worksheet');
    signal?.throwIfAborted();
    return { zip, sheets, activeTab };
}

async function worksheetParts(archive, selected, signal) {
    const omitted = new Set(archive.sheets.filter(sheet => sheet !== selected).map(sheet => sheet.target));
    const keep = new Set(['[Content_Types].xml', 'xl/workbook.xml']);
    const pending = ['xl/workbook.xml'];
    while (pending.length) {
        signal?.throwIfAborted();
        const part = pending.pop();
        const separator = part.lastIndexOf('/');
        const relationshipPart = `${part.slice(0, separator + 1)}_rels/${part.slice(separator + 1)}.rels`;
        const file = archive.zip.file(relationshipPart);
        if (!file) continue;
        keep.add(relationshipPart);
        const sax = parser();
        sax.on('opentag', tag => {
            if (tag.uri !== packageNamespace || tag.local !== 'Relationship' ||
                attribute(tag, 'TargetMode') === 'External' || attribute(tag, 'Type') === `${relationshipNamespace}/hyperlink`) return;
            const target = resolveWorkbookPart(part, attribute(tag, 'Target'));
            if (omitted.has(target) || keep.has(target) || !archive.zip.file(target)) return;
            keep.add(target);
            pending.push(target);
        });
        sax.write(await file.async('string')).close();
    }
    return keep;
}

export async function projectWorksheet(archive, index, rowLimit, signal) {
    signal?.throwIfAborted();
    const selected = archive.sheets[index];
    if (!selected) throw new RangeError('Invalid worksheet index');
    const zip = archive.zip.clone();
    // JSZip.clone() shares its entry index; preserve the source for subsequent sheet changes.
    zip.files = { ...archive.zip.files };
    const keep = await worksheetParts(archive, selected, signal);
    // Removing a JSZip directory recursively would also remove retained descendants.
    for (const name of Object.keys(zip.files)) if (!keep.has(name)) delete zip.files[name];
    let projection;
    if (rowLimit !== undefined) {
        projection = projectWorksheetRows(await zip.file(selected.target).async('string'), rowLimit);
        zip.file(selected.target, projection.xml);
    }
    const bytes = await zip.generateAsync({ type: 'uint8array', compression: 'STORE' });
    signal?.throwIfAborted();
    return { bytes, rows: projection?.rows, columns: projection?.columns, rowSettings: projection?.rowSettings,
        drawingIds: projection?.drawingIds ?? [], truncated: projection?.truncated ?? false };
}
