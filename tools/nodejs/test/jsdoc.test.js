/**
 * Intended to be similar to stubtest for python
 */

var duckdb = require("..");
var jsdoc = require("jsdoc3-parser");
const { expect } = require('chai');
const { promisify } = require('util');
const { writeFile } = require('fs/promises');

function lastDot(string) {
  const array = string.split('.');
  return array[array.length - 1];
}

/**
 * @typedef {Object} Node
 * @property {string} name
 * @property {string} memberof
 * @property {string} longname
 * @property {string} scope
 */

describe("JSDoc contains all methods", () => {
  /**
   * @type {Node[]}
   */
  let docs;
  before(async () => {
    docs = await promisify(jsdoc)(require.resolve("../lib/duckdb"));

    await writeFile(
      'jsdocout.json',
      JSON.stringify(docs, undefined, 2)
    );
  })

  function checkDocs(obj, scope) {
    const symbols = Object.getOwnPropertySymbols(obj).map(i => lastDot(i.description));
    const expected = Object
      .getOwnPropertyNames(obj)
      .concat(symbols)
      .sort()
      .filter(name => name !== 'constructor');

    const actual = docs
      .filter((node) => node.memberof === scope && !node.undocumented)
      .map((node) => lastDot(node.name))
      .sort();

    expect(expected).to.deep.equals(actual, 'items missing from documentation');
  }

  for (const clazz of ['Database', 'QueryResult', 'Connection', 'Statement']) {
    it(clazz, () => {
      checkDocs(duckdb[clazz].prototype, `module:duckdb~${clazz}`);
    });
  }

  it('module root', () => {
    checkDocs(duckdb, 'module:duckdb');
  });
});
