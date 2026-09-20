import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { Worker } from 'node:worker_threads';

const require = createRequire(import.meta.url);
const ExcelJS = require('../../src/Glance.Components/Office/third_party/web/exceljs/4.4.0/dist/exceljs.bare.js');
const JSZip = require('../../src/Glance.Components/Office/third_party/web/jszip/3.10.1/dist/jszip.min.js');
const book = new ExcelJS.Workbook();
book.addWorksheet('Hidden', { state: 'hidden' }).getCell('A1').value = 'Hidden';
const data = book.addWorksheet('Data');
data.getCell('A1').value = 'First';
data.getCell('AZ10000').value = 'Last';
data.getCell('B2').value = { formula: '1+2', result: 3 };
data.getColumn(4).numFmt = '0.00%';
data.getRow(3).font = { name: 'Arial', bold: true };
data.getCell('D3').value = 0.25;
data.getCell('E4').value = new Date('2020-02-03T00:00:00Z');
data.getCell('E4').numFmt = 'yyyy-mm-dd';
data.getCell('C70').value = 'Merged tail';
data.mergeCells('C70:E72');
data.fillFormula('F1:F3', 'B1*2', [0, 6, 0]);
const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a4SUAAAAASUVORK5CYII=', 'base64');
const imageId = book.addImage({ buffer: png, extension: 'png' });
data.addImage(imageId, { tl: { col: 1, row: 40 }, ext: { width: 100, height: 80 } });
book.addWorksheet('Empty');
const pictures = book.addWorksheet('Pictures');
const secondPng = Buffer.concat([png, Buffer.from('second image')]);
const secondImageId = book.addImage({ buffer: secondPng, extension: 'png' });
pictures.addImage(imageId, { tl: { col: 0, row: 0 }, ext: { width: 100, height: 80 } });
pictures.addImage(secondImageId, { tl: { col: 2, row: 0 }, ext: { width: 100, height: 80 } });
book.views = [{ activeTab: 1 }];
const zip = await JSZip.loadAsync(await book.xlsx.writeBuffer());
const relation = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships';
const chartXml = '<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart"><c:chart><c:plotArea/></c:chart></c:chartSpace>';
zip.file('xl/charts/chart1.xml', chartXml);
zip.file('xl/drawings/drawing1.xml', (await zip.file('xl/drawings/drawing1.xml').async('string')).replace('</xdr:wsDr>',
    `<xdr:oneCellAnchor><xdr:from><xdr:col>3</xdr:col><xdr:row>2</xdr:row></xdr:from><xdr:ext cx="3810000" cy="2857500"/><xdr:graphicFrame><a:graphic><a:graphicData><c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" xmlns:r="${relation}" r:id="chart"/></a:graphicData></a:graphic></xdr:graphicFrame><xdr:clientData/></xdr:oneCellAnchor></xdr:wsDr>`));
zip.file('xl/drawings/_rels/drawing1.xml.rels', (await zip.file('xl/drawings/_rels/drawing1.xml.rels').async('string')).replace('</Relationships>',
    `<Relationship Id="chart" Type="${relation}/chart" Target="../charts/chart1.xml"/></Relationships>`));
const path = 'xl/worksheets/sheet2.xml';
zip.file(path, (await zip.file(path).async('string')).replace('</worksheet>',
    '<dataValidations count="1"><dataValidation type="whole" sqref="A1:XFD1048576"><formula1>0</formula1></dataValidation></dataValidations></worksheet>'));
const originalBytes = await zip.generateAsync({ type: 'uint8array' });
// Change namespace aliases in the controlled fixture without changing its data.
for (const entry of Object.values(zip.files)) {
    if (entry.dir || !/\.(xml|rels)$/.test(entry.name)) continue;
    let xml = await entry.async('string');
    if (/^xl\/(workbook\.xml|styles\.xml|sharedStrings\.xml|worksheets\/sheet\d+\.xml)$/.test(entry.name)) {
        xml = xml.replace(/<(\/?)([A-Za-z][A-Za-z0-9]*)(?=[\s/>])/g, '<$1sheet:$2')
            .replace('xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"',
                'xmlns:sheet="http://schemas.openxmlformats.org/spreadsheetml/2006/main"');
    } else if (entry.name.endsWith('.rels')) {
        xml = xml.replace(/<(\/?)(Relationships|Relationship)(?=[\s/>])/g, '<$1package:$2')
            .replace('xmlns="http://schemas.openxmlformats.org/package/2006/relationships"',
                'xmlns:package="http://schemas.openxmlformats.org/package/2006/relationships"');
    }
    xml = xml.replaceAll('xmlns:r=', 'xmlns:link=').replaceAll('r:id=', 'link:id=')
        .replaceAll('r:embed=', 'link:embed=')
        .replaceAll('xmlns:xdr=', 'xmlns:drawing=').replaceAll('<xdr:', '<drawing:').replaceAll('</xdr:', '</drawing:')
        .replaceAll('xmlns:a=', 'xmlns:shape=').replaceAll('<a:', '<shape:').replaceAll('</a:', '</shape:');
    zip.file(entry.name, xml);
}
const bytes = await zip.generateAsync({ type: 'uint8array' });
const workerUrl = new URL('../../src/Glance.Components/Office/third_party/web/bin/workbook-worker.mjs', import.meta.url).href;
const worker = new Worker(`
    const { parentPort } = require('node:worker_threads');
    globalThis.self = globalThis;
    globalThis.postMessage = value => parentPort.postMessage(value);
    import(${JSON.stringify(workerUrl)}).then(() => {
        parentPort.on('message', data => self.onmessage({ data }));
        parentPort.postMessage({ type: 'initialized' });
    });
`, { eval: true, resourceLimits: { maxOldGenerationSizeMb: 128 } });
const events = [];
const waiters = new Set();
let fatal;
worker.on('message', data => {
    events.push(data);
    for (const item of [...waiters]) if (item.match(data)) { waiters.delete(item); item.resolve(data); }
});
worker.on('error', error => {
    fatal = error;
    for (const item of waiters) item.reject(error);
    waiters.clear();
});
function wait(match) {
    if (fatal) return Promise.reject(fatal);
    const previous = events.find(match);
    if (previous) return Promise.resolve(previous);
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => { waiters.delete(item); reject(new Error('Workbook response timed out')); }, 10000);
        const item = { match, resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); } };
        waiters.add(item);
    });
}
try {
    await wait(event => event.type === 'initialized');
    worker.postMessage({ command: 'open', bytes });
    const first = await wait(event => ['first', 'error'].includes(event.type));
    assert.equal(first.type, 'first', first.message);
    assert.equal(first.index, 1);
    assert.equal(first.metadata.rows, 10000);
    assert.equal(first.metadata.columns, 52);
    assert.equal(first.metadata.images.length, 1);
    assert.equal(first.metadata.images[0].range.tl.nativeRow, 40);
    assert.equal(first.metadata.charts.length, 1);
    assert.deepEqual(first.metadata.charts[0].range.ext, { width: 400, height: 300 });
    worker.postMessage({ command: 'chart', generation: 1, id: first.metadata.charts[0].id, request: 91 });
    const chart = await wait(event => event.type === 'chart' || event.type === 'error');
    assert.equal(chart.type, 'chart', chart.message);
    assert.equal(chart.request, 91);
    assert.equal(chart.parts.xml, chartXml);
    assert.equal(chart.parts.path, 'xl/charts/chart1.xml');
    const complete = await wait(event => ['complete', 'error'].includes(event.type));
    assert.equal(complete.type, 'complete', complete.message);
    worker.postMessage({ command: 'range', generation: 1, id: 1, rows: [2, 10000], columns: [2, 52] });
    const range = await wait(event => event.type === 'range');
    assert.equal(range.rows[0].cells[0].result, 3);
    assert.equal(range.rows[1].cells[0].value, 'Last');
    worker.postMessage({ command: 'range', generation: 1, id: 2, rows: [3, 4, 70, 71], columns: [3, 4, 5, 6] });
    const preserved = await wait(event => event.type === 'range' && event.id === 2);
    const expectedBook = new ExcelJS.Workbook();
    await expectedBook.xlsx.load(originalBytes, { ignoreNodes: ['dataValidations'] });
    const expectedSheet = expectedBook.getWorksheet('Data');
    for (const row of preserved.rows) for (const cell of row.cells) {
        const { column, ...model } = cell;
        assert.deepEqual(model, expectedSheet.getCell(row.index, column).model);
    }
    assert.equal(complete.metadata.merges[0].cell.value, 'Merged tail');
    worker.postMessage({ command: 'image', generation: 1, id: first.metadata.images[0].id });
    const image = await wait(event => event.type === 'image');
    assert.equal(image.extension, 'png');
    assert.deepEqual(Buffer.from(image.bytes), png);
    for (const index of [2, 1, 2]) worker.postMessage({ command: 'select', index });
    const empty = await wait(event => event.generation === 4 && ['complete', 'error'].includes(event.type));
    assert.equal(empty.type, 'complete', empty.message);
    assert.equal(empty.metadata.rows, 0);
    assert.equal(empty.metadata.columns, 0);
    assert.equal(empty.metadata.charts.length, 0);
    worker.postMessage({ command: 'select', index: 3 });
    const pictureSheet = await wait(event => event.generation === 5 && ['complete', 'error'].includes(event.type));
    assert.equal(pictureSheet.type, 'complete', pictureSheet.message);
    assert.equal(pictureSheet.metadata.images.length, 2);
    for (const [index, expected] of [png, secondPng].entries()) {
        const id = pictureSheet.metadata.images[index].id;
        worker.postMessage({ command: 'image', generation: 5, id });
        const loaded = await wait(event => event.generation === 5 && event.type === 'image' && event.id === id);
        assert.deepEqual(Buffer.from(loaded.bytes), expected);
    }
    worker.postMessage({ command: 'select', index: 1 });
    const restored = await wait(event => event.generation === 6 && ['complete', 'error'].includes(event.type));
    assert.equal(restored.type, 'complete', restored.message);
    assert.equal(restored.metadata.images.length, 1);
    assert.equal(restored.metadata.charts.length, 1);
    assert.equal(restored.metadata.merges[0].cell.value, 'Merged tail');
    assert.equal(events.filter(event => event.type === 'error').length, 0);
    console.log('Office workbook worker regression tests passed');
} finally { await worker.terminate(); }
