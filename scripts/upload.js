const { Storage } = require('@google-cloud/storage')
const fs = require('fs')
const path = require('path')

const BUCKET_NAME = process.env.BUCKET_NAME || 'cudo-download'
const PREBUILDS_PATH = process.env.PREBUILDS_PATH || 'prebuilds'
const VERSION = process.env.CI_COMMIT_TAG || process.VERSION || process.argv[2]

if (!VERSION) {
  throw new Error('no upload version specified')
}

const storage = new Storage()
const bucket = storage.bucket(BUCKET_NAME)

fs.readdir(path.resolve(PREBUILDS_PATH), async (err, files) => {
  if (err) {
    throw err
  }

  let found = false
  for (const file of files) {
    if (file.endsWith('.tar.gz')) {
      const parts = file.replace('.tar.gz', '').split('-')
      const lastParts = parts.slice(-4)
      const vPos = file.indexOf('-v')
      const name = file.substr(0, vPos)
      const version = file.slice(vPos + 2, file.indexOf(lastParts[0]) - 1)
      const runtime = lastParts[0]
      const abi = lastParts[1]
      const arch = lastParts[3]

      if (`v${version}` !== VERSION) {
        continue
      }

      console.log(`found ${path.join(PREBUILDS_PATH, file)}`)
      found = true

      let platform
      switch (lastParts[2]) {
        case 'darwin':
          platform = 'mac'
          break
        case 'win32':
          platform = 'win'
          break
        case 'linux':
          platform = 'linux'
          break
      }
      if (!platform) {
        console.warn(`unknown platform ${platform}`)
        continue
      }

      const destination = `/images/${name}/${platform}/${arch}/${version}-${runtime}-${abi}.tar.gz`
      console.log(`uploading to gs://${BUCKET_NAME}/${destination}...`)

      const upload = await bucket.upload(path.resolve(PREBUILDS_PATH, file), { destination })
      console.log(`completed with content type ${upload[0].metadata.contentType} and size ${upload[0].metadata.size}\n`)
    }
  }

  if (!found) {
    throw new Error('no prebuilds matching version ${VERSION} found')
  }
})
