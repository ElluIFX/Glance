import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { openWorkbookArchive, projectWorksheet, projectWorksheetRows, workbookThemeColors } from '../../src/Glance.Components/Office/third_party/web/bin/workbook-archive.mjs';
const require = createRequire(import.meta.url);
const JSZip = require('../../src/Glance.Components/Office/third_party/web/jszip/3.10.1/dist/jszip.min.js');
const ns = 'http://schemas.openxmlformats.org/spreadsheetml/2006/main';
const rel = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships';
const packageNs = 'http://schemas.openxmlformats.org/package/2006/relationships';
const theme = '<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><a:themeElements><a:clrScheme><a:dk1><a:sysClr val="windowText" lastClr="112233"/></a:dk1><a:lt1><a:srgbClr val="FFFFFF"/></a:lt1><a:accent1><a:srgbClr val="336699"/></a:accent1></a:clrScheme></a:themeElements></a:theme>';
assert.equal(workbookThemeColors(theme)[0], 'FFFFFF');
assert.equal(workbookThemeColors(theme)[1], '112233');
assert.equal(workbookThemeColors(theme)[4], '336699');
assert.deepEqual(workbookThemeColors(undefined), ['FFFFFF', '000000']);
assert.throws(() => workbookThemeColors(`<!DOCTYPE root>${theme}`), /Document types/);
const wrap = (body, tail = '') => `<s:worksheet xmlns:s="${ns}"><s:sheetData>${body}</s:sheetData>${tail}</s:worksheet>`;
const row = number => `<s:row r="${number}"><s:c r="A${number}" t="inlineStr"><s:is><s:t>One &amp; two</s:t></s:is></s:c></s:row>`;
const sourceXml = wrap(row(1) + row(100));
const projected = projectWorksheetRows(sourceXml, 60);
assert.equal(projected.xml, wrap(row(1)));
assert.equal(projected.rows, 100);
assert.equal(projected.columns, 1);
assert.equal(projected.truncated, true);
assert.equal(projectWorksheetRows(`<?xml version="1.0"?>\n${sourceXml}\n`).xml, projected.xml);
assert.equal(projectWorksheetRows(wrap('')).truncated, false);
const merged = projectWorksheetRows(wrap(row(1), '<s:mergeCells><s:mergeCell ref="A1:B100"/></s:mergeCells>'));
assert.equal(merged.rows, 100);
assert.equal(merged.columns, 2);
assert.equal(merged.truncated, true);
assert(merged.xml.includes('ref="A1:B60"'));
assert.throws(() => projectWorksheetRows(wrap('', '<s:mergeCells><s:mergeCell ref="A1:XFD1048576"/></s:mergeCells>')), /budget/);
assert.throws(() => projectWorksheetRows(wrap('', '<s:mergeCells><s:mergeCell ref="B2:A1"/></s:mergeCells>')), /bounds/);
assert.throws(() => projectWorksheetRows(sourceXml, 0), RangeError);
assert.throws(() => projectWorksheetRows('<root/>'), /missing/);
assert.throws(() => projectWorksheetRows(`<!DOCTYPE root>${sourceXml}`), /Document types/);
assert.throws(() => projectWorksheetRows(sourceXml.replace('r="100"', 'r="1048577"')), /row/);
assert.throws(() => projectWorksheetRows(sourceXml.replace('</s:worksheet>', '<s:sheetData/></s:worksheet>')), /Duplicate/);
const distant = projectWorksheetRows(wrap('<s:row r="10000" ht="40" hidden="1"><s:c r="AZ10000"><s:v>1</s:v></s:c></s:row>'));
assert.equal(distant.columns, 52);
assert.deepEqual(distant.rowSettings, [{ index: 10000, height: 40, hidden: true }]);

async function fixture(target = 'worksheets/sheet9.xml', extra = '') {
    const zip = new JSZip();
    zip.file('xl/workbook.xml', `<workbook xmlns="${ns}" xmlns:r="${rel}"><bookViews><workbookView activeTab="2"/></bookViews><sheets><sheet name="Hidden" sheetId="1" state="hidden" r:id="one"/><sheet name="First" sheetId="4" r:id="two"/><sheet name="Last &amp; final" sheetId="9" r:id="three"/></sheets></workbook>`);
    zip.file('xl/_rels/workbook.xml.rels', `<Relationships xmlns="${packageNs}"><Relationship Id="one" Type="${rel}/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="two" Type="${rel}/worksheet" Target="worksheets/sheet4.xml"/><Relationship Id="three" Type="${rel}/worksheet" Target="${target}" ${extra}/></Relationships>`);
    for (const id of [1, 4, 9]) zip.file(`xl/worksheets/sheet${id}.xml`, sourceXml);
    return zip.generateAsync({ type: 'uint8array' });
}
const archive = await openWorkbookArchive(await fixture());
assert.equal(archive.activeTab, 2);
assert.equal(archive.sheets[2].name, 'Last & final');
const first = await projectWorksheet(archive, 2, 60);
assert.equal(first.rows, 100);
const firstZip = await JSZip.loadAsync(first.bytes);
assert.equal(firstZip.file('xl/worksheets/sheet1.xml'), null);
assert.equal(await firstZip.file('xl/worksheets/sheet9.xml').async('string'), projected.xml);
const complete = await JSZip.loadAsync((await projectWorksheet(archive, 0)).bytes);
assert.equal(await complete.file('xl/worksheets/sheet1.xml').async('string'), sourceXml);
assert.equal(await archive.zip.file('xl/worksheets/sheet9.xml').async('string'), sourceXml);
const relationshipXml = links => `<Relationships xmlns="${packageNs}">${links.map(([id, type, target, mode]) =>
    `<Relationship Id="${id}" Type="${rel}/${type}" Target="${target}"${mode ? ` TargetMode="${mode}"` : ''}/>`).join('')}</Relationships>`;
archive.zip.file('xl/worksheets/_rels/sheet9.xml.rels', relationshipXml([
    ['drawing', 'drawing', '../drawings/drawing1.xml'],
    ['link', 'hyperlink', '#LocalName'],
    ['external', 'image', 'https://example.com/image.png', 'External'],
    ['missing', 'image', '../media/missing.png'],
]));
archive.zip.file('xl/media/_rels/missing.png.rels', '<!DOCTYPE missing><invalid/>');
archive.zip.file('xl/worksheets/_rels/sheet4.xml.rels', relationshipXml([['drawing', 'drawing', '../drawings/drawing2.xml']]));
archive.zip.file('xl/drawings/drawing1.xml', '<drawing/>');
archive.zip.file('xl/drawings/drawing2.xml', '<drawing/>');
archive.zip.file('xl/drawings/_rels/drawing1.xml.rels', relationshipXml([
    ['image', 'image', '../media/shared.png'], ['chart', 'chart', '../charts/chart1.xml'],
]));
archive.zip.file('xl/drawings/_rels/drawing2.xml.rels', relationshipXml([
    ['shared', 'image', '../media/shared.png'], ['unique', 'image', '../media/other.png'],
]));
archive.zip.file('xl/charts/chart1.xml', '<chart/>');
archive.zip.file('xl/charts/_rels/chart1.xml.rels', relationshipXml([['cycle', 'drawing', '../drawings/drawing1.xml']]));
archive.zip.file('xl/media/shared.png', new Uint8Array([1, 2, 3]));
archive.zip.file('xl/media/other.png', new Uint8Array([4, 5, 6]));
const otherImage = archive.zip.file('xl/media/other.png');
const originalRead = otherImage.async;
otherImage.async = () => { throw new Error('Unselected worksheet resource was read'); };
const selectedZip = await JSZip.loadAsync((await projectWorksheet(archive, 2, 60)).bytes);
assert.equal(selectedZip.file('xl/media/other.png'), null);
assert.equal(selectedZip.file('xl/drawings/drawing2.xml'), null);
assert.equal(selectedZip.file('xl/worksheets/_rels/sheet4.xml.rels'), null);
assert.deepEqual(await selectedZip.file('xl/media/shared.png').async('uint8array'), new Uint8Array([1, 2, 3]));
assert.equal(await selectedZip.file('xl/charts/chart1.xml').async('string'), '<chart/>');
assert(selectedZip.file('xl/charts/_rels/chart1.xml.rels'));
otherImage.async = originalRead;
const switchedZip = await JSZip.loadAsync((await projectWorksheet(archive, 1, 60)).bytes);
assert.deepEqual(await switchedZip.file('xl/media/other.png').async('uint8array'), new Uint8Array([4, 5, 6]));
assert(switchedZip.file('xl/media/shared.png'));
assert.equal(switchedZip.file('xl/charts/chart1.xml'), null);
assert(archive.zip.file('xl/charts/chart1.xml'));
await assert.rejects(async () => openWorkbookArchive(await fixture('../../escape.xml')), /escapes/);
await assert.rejects(async () => openWorkbookArchive(await fixture('https://example.com/book.xml', 'TargetMode="External"')), /External/);
const cancelled = new AbortController();
cancelled.abort();
await assert.rejects(() => projectWorksheet(archive, 0, 60, cancelled.signal), { name: 'AbortError' });
console.log('Office workbook archive regression tests passed');
