import { SaxesParser } from 'saxes';

const wordNamespaces = new Set([
    'http://schemas.openxmlformats.org/wordprocessingml/2006/main',
    'http://purl.oclc.org/ooxml/wordprocessingml/main',
]);

function escapeXml(value) {
    return value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;').replaceAll('\r', '&#13;')
        .replaceAll('\n', '&#10;').replaceAll('\t', '&#9;');
}

// Preserve complete body children and section properties in a valid first-view document.
export function projectDocumentBody(xml, blockLimit = 40) {
    if (!Number.isSafeInteger(blockLimit) || blockLimit < 1) {
        throw new RangeError('Invalid document block limit');
    }
    const parser = new SaxesParser({ xmlns: true });
    const output = [];
    let depth = 0;
    let bodyDepth = -1;
    let skippedDepth = 0;
    let blocks = 0;
    let foundBody = false;
    parser.on('doctype', () => { throw new Error('Document types are not supported'); });
    parser.on('opentag', tag => {
        ++depth;
        if (tag.local === 'altChunk' && wordNamespaces.has(tag.uri)) {
            throw new Error('Embedded alternate documents require another renderer');
        }
        if (skippedDepth) {
            ++skippedDepth;
            return;
        }
        if (depth === 2 && tag.local === 'body' && wordNamespaces.has(tag.uri)) {
            if (foundBody) throw new Error('Duplicate document body');
            foundBody = true;
            bodyDepth = depth;
        }
        if (bodyDepth >= 0 && depth === bodyDepth + 1 &&
            !(tag.local === 'sectPr' && wordNamespaces.has(tag.uri)) &&
            ++blocks > blockLimit) {
            skippedDepth = 1;
            return;
        }
        output.push('<', tag.name);
        for (const attribute of Object.values(tag.attributes)) {
            output.push(' ', attribute.name, '="', escapeXml(attribute.value), '"');
        }
        output.push('>');
    });
    parser.on('closetag', tag => {
        if (skippedDepth) --skippedDepth;
        else output.push('</', tag.name, '>');
        if (depth === bodyDepth) bodyDepth = -1;
        --depth;
    });
    parser.on('text', value => { if (!skippedDepth && depth > 0) output.push(escapeXml(value)); });
    parser.on('cdata', value => { if (!skippedDepth) output.push(escapeXml(value)); });
    parser.write(xml).close();
    if (!foundBody) throw new Error('Document body is missing');
    return { xml: output.join(''), truncated: blocks > blockLimit };
}
