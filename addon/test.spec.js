const index = require('./')
const assert = require('assert')
const { describe, it } = require('mocha')

const exampleLogs = [
  {
    should: 'parse hash rate messages',
    message: 'Totals (ALL):    825.7  697.8    0.0 H/s',
    expected: { type: 'hashRate', hashRate: 825.7 }
  },
  {
    should: 'parse error messages',
    message: '[2019-11-25 15:27:28] : ERROR: no miner backend enabled.',
    expected: { type: 'error', error: 'no miner backend enabled.' }
  },
  {
    should: 'parse log messages',
    message: '[2019-11-25 15:27:28] : WARNING on macOS thread affinity is only advisory.',
    expected: { type: 'log', message: 'WARNING on macOS thread affinity is only advisory.' }
  }
]

describe('cudo-xmr-stak/1.0.0', () => {
  exampleLogs.forEach(log => {
    it(log.should, () => {
      const module = index()
      const parsedMessage = module.parseLog(log.message)
      assert.deepStrictEqual(parsedMessage, log.expected)
    })
  })
})
