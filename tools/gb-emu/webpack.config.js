const path = require('path');

module.exports = {
  mode: 'production',
  entry: './src/index.ts',
  // devtool: 'inline-source-map',
  module: {
    rules: [
      {
        test: /\.tsx?$/,
        use: 'ts-loader',
        exclude: /node_modules/
      }
    ]
  },
  resolve: {
    extensions: [ '.ts', '.js' ]
  },
  devServer: {
    contentBase: path.join(__dirname, "dist"),
    port: 8080,
    // Carte SD servie par le serveur : SD_DIR=/chemin npm start
    // (défaut : roms/meta, les dossiers de jeux faits par compile.sh).
    before(app) {
      const fs = require('fs');
      const root = process.env.SD_DIR
        ? path.resolve(process.env.SD_DIR)
        : path.resolve(__dirname, '..', '..', 'roms', 'meta');
      app.get('/sd/tree', (_req, res) => {
        if (!fs.existsSync(root)) {
          res.status(404).json({ error: 'pas de carte : lance avec SD_DIR=/chemin/de/la/carte npm start' });
          return;
        }
        const walk = (dir, rel) => fs.readdirSync(dir, { withFileTypes: true })
          .filter((e) => !e.name.startsWith('.'))
          .map((e) => e.isDirectory()
            ? { name: e.name, children: walk(path.join(dir, e.name), rel + '/' + e.name) }
            : { name: e.name, size: fs.statSync(path.join(dir, e.name)).size, path: rel + '/' + e.name });
        res.json(walk(root, ''));
      });
      app.get('/sd/file', (req, res) => {
        const full = path.resolve(root, String(req.query.p || ''));
        if (full.startsWith(root + path.sep) && fs.existsSync(full) && fs.statSync(full).isFile()) {
          res.sendFile(full);
        } else {
          res.status(404).end();
        }
      });
    }
  },
  output: {
    filename: 'meta-emulator.js',
    path: path.resolve(__dirname, 'dist'),
    library: 'metaEmulator'
  }
};