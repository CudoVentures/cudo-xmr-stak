const { fork } = require('child_process')
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
    const parts = message.split('|')
    const log = parts.slice(2).join(' ').trim()

    if (parts[0] === 'ERR') {
      return { type: 'error', error: log }
    } else if (parts[0] === 'RES' && parts[2] === 'speed') {
      const hashRate = parseFloat(parts[3] || 0) || 0
      return { type: 'hashRate', hashRate }
    } else {
      return { type: 'log', message: log }
    }
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

    if (ctx.workload.platform === 'win') {
      env.PATH = `${env.PATH};${ctx.workloadDir}`
    } else if (ctx.workload.platform === 'linux') {
      env.LD_LIBRARY_PATH = `$LD_LIBRARY_PATH:${ctx.workloadDir}`
    }

    const params = [
      '-o', `${ctx.workload.host}:${ctx.workload.port}`,
      '-u', ctx.poolUser,
      '-a', ctx.workload.algorithmId === 'cryptonight-lite' ? 'cryptonight-lite' : 'cryptonight'
    ]

    if (ctx.workloadSettings['cpu-priority'] !== undefined) {
      params.push('--cpu-priority', ctx.workloadSettings['cpu-priority'])
    }
    if (ctx.workloadSettings['cpu-threads'] !== undefined) {
      params.push('--threads', ctx.workloadSettings['cpu-threads'])
    }

    try {
      const executable = path.resolve(ctx.workloadDir, 'build', 'Release', `cudo.node`)
      const child = path.resolve(ctx.workloadDir, 'build', 'Release', 'child.js')
      fs.accessSync(executable, fs.constants.R_OK)
      fs.accessSync(child, fs.constants.R_OK)
      module.proc = fork(child, [executable, ...params], {
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
    module.proc.stderr.on('data', data => module.readLog(data))

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
