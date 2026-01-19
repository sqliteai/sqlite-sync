#!/usr/bin/env node

/**
 * Generates the @sqliteai/sqlite-sync-expo package
 *
 * This script creates an npm package that bundles CloudSync binaries
 * for Expo apps, with an Expo config plugin for automatic setup.
 *
 * Usage:
 *   node generate-expo-package.js <version> <artifacts-dir> <output-dir>
 *
 * Example:
 *   node generate-expo-package.js 0.8.53 ./artifacts ./expo-package
 */

const fs = require('fs');
const path = require('path');

// Android architectures to include
const ANDROID_ARCHS = [
  { name: 'arm64-v8a', artifact: 'cloudsync-android-arm64-v8a' },
  { name: 'armeabi-v7a', artifact: 'cloudsync-android-armeabi-v7a' },
  { name: 'x86_64', artifact: 'cloudsync-android-x86_64' },
];

/**
 * Generate package.json
 */
function generatePackageJson(version) {
  return {
    name: '@sqliteai/sqlite-sync-expo',
    version: version,
    description: 'SQLite Sync extension for Expo - Sync on-device databases with SQLite Cloud',
    main: 'src/index.js',
    types: 'src/index.d.ts',
    files: [
      'src',
      'android',
      'ios',
      'app.plugin.js',
    ],
    keywords: [
      'react-native',
      'expo',
      'expo-plugin',
      'sqlite',
      'sqlite-sync',
      'sync',
      'offline-first',
      'database-sync',
    ],
    author: 'SQLite AI',
    license: 'SEE LICENSE IN LICENSE.md',
    repository: {
      type: 'git',
      url: 'https://github.com/sqliteai/sqlite-sync.git',
      directory: 'packages/expo',
    },
    homepage: 'https://github.com/sqliteai/sqlite-sync#react-native--expo',
    bugs: {
      url: 'https://github.com/sqliteai/sqlite-sync/issues',
    },
    peerDependencies: {
      expo: '>=51.0.0',
      'react-native': '>=0.73.0',
    },
    peerDependenciesMeta: {
      expo: {
        optional: true,
      },
    },
  };
}

/**
 * Generate src/index.js
 */
function generateIndexJs() {
  return `/**
 * @sqliteai/sqlite-sync-expo
 *
 * SQLite Sync extension binaries for Expo.
 * This package provides pre-built binaries and an Expo config plugin.
 *
 * Usage:
 * 1. Add to app.json plugins: ["@sqliteai/sqlite-sync-expo"]
 * 2. Run: npx expo prebuild --clean
 * 3. Load extension in your code (see README)
 */

module.exports = {
  // Package metadata
  name: '@sqliteai/sqlite-sync-expo',

  // Extension identifiers for loading
  ios: {
    bundleId: 'ai.sqlite.cloudsync',
    frameworkName: 'CloudSync',
  },
  android: {
    libraryName: 'cloudsync',
  },
};
`;
}

/**
 * Generate src/index.d.ts
 */
function generateIndexDts() {
  return `declare module '@sqliteai/sqlite-sync-expo' {
  export const name: string;

  export const ios: {
    bundleId: string;
    frameworkName: string;
  };

  export const android: {
    libraryName: string;
  };
}
`;
}

/**
 * Generate app.plugin.js (Expo config plugin)
 */
function generateAppPlugin() {
  return `/**
 * Expo Config Plugin for SQLite Sync Extension
 *
 * This plugin automatically configures iOS and Android to include the SQLite Sync
 * native binaries. Just add "@sqliteai/sqlite-sync-expo" to your app.json plugins.
 *
 * Usage in app.json:
 * {
 *   "expo": {
 *     "plugins": ["@sqliteai/sqlite-sync-expo"]
 *   }
 * }
 */

const {
  withXcodeProject,
  withDangerousMod,
  IOSConfig,
} = require('expo/config-plugins');
const fs = require('fs');
const path = require('path');

// Get the directory where this package is installed
const PACKAGE_ROOT = __dirname;

/**
 * iOS: Add CloudSync.xcframework to the Xcode project
 */
function withSqliteSyncIOS(config) {
  return withXcodeProject(config, (config) => {
    const projectRoot = config.modRequest.projectRoot;
    const projectName = config.modRequest.projectName;
    const xcodeProject = config.modResults;

    // Source path: xcframework bundled with this package
    const srcFrameworkPath = path.join(PACKAGE_ROOT, 'ios', 'CloudSync.xcframework');

    // Destination path: inside the iOS project
    const destFrameworkPath = path.join(
      projectRoot,
      'ios',
      projectName,
      'CloudSync.xcframework'
    );

    // Check source exists
    if (!fs.existsSync(srcFrameworkPath)) {
      throw new Error(
        \`CloudSync.xcframework not found at \${srcFrameworkPath}. \` +
        'This is a bug in @sqliteai/sqlite-sync-expo - the package is missing iOS binaries.'
      );
    }

    // Copy xcframework to iOS project directory
    console.log(\`[@sqliteai/sqlite-sync-expo] Copying xcframework to \${destFrameworkPath}\`);
    fs.cpSync(srcFrameworkPath, destFrameworkPath, { recursive: true });

    // Get the main app target
    const target = IOSConfig.XcodeUtils.getApplicationNativeTarget({
      project: xcodeProject,
      projectName: projectName,
    });

    // Check if "Embed Frameworks" build phase exists, create if not
    const embedFrameworksBuildPhase = xcodeProject.buildPhaseObject(
      'PBXCopyFilesBuildPhase',
      'Embed Frameworks'
    );

    if (!embedFrameworksBuildPhase) {
      console.log('[@sqliteai/sqlite-sync-expo] Creating "Embed Frameworks" build phase');
      xcodeProject.addBuildPhase(
        [],
        'PBXCopyFilesBuildPhase',
        'Embed Frameworks',
        target.uuid,
        'frameworks'
      );
    }

    // Add the framework to the project
    const relativePath = \`\${projectName}/CloudSync.xcframework\`;
    console.log(\`[@sqliteai/sqlite-sync-expo] Adding framework: \${relativePath}\`);

    xcodeProject.addFramework(relativePath, {
      target: target.uuid,
      customFramework: true,
      embed: true,
      sign: true,
      link: true,
    });

    console.log('[@sqliteai/sqlite-sync-expo] iOS setup complete');
    return config;
  });
}

/**
 * Android: Copy libcloudsync.so files to jniLibs
 */
function withSqliteSyncAndroid(config) {
  return withDangerousMod(config, [
    'android',
    async (config) => {
      const projectRoot = config.modRequest.projectRoot;

      // Source directory: .so files bundled with this package
      const srcDir = path.join(PACKAGE_ROOT, 'android', 'jniLibs');

      // Destination: android/app/src/main/jniLibs
      const jniLibsDir = path.join(
        projectRoot,
        'android',
        'app',
        'src',
        'main',
        'jniLibs'
      );

      // Architectures we support
      const architectures = ['arm64-v8a', 'armeabi-v7a', 'x86_64'];

      for (const arch of architectures) {
        const srcFile = path.join(srcDir, arch, 'cloudsync.so');
        const destDir = path.join(jniLibsDir, arch);
        const destFile = path.join(destDir, 'cloudsync.so');

        // Check source exists
        if (!fs.existsSync(srcFile)) {
          console.warn(
            \`[@sqliteai/sqlite-sync-expo] Warning: \${srcFile} not found, skipping \${arch}\`
          );
          continue;
        }

        // Create destination directory
        fs.mkdirSync(destDir, { recursive: true });

        // Copy the .so file
        console.log(\`[@sqliteai/sqlite-sync-expo] Copying \${arch}/cloudsync.so\`);
        fs.copyFileSync(srcFile, destFile);
      }

      console.log('[@sqliteai/sqlite-sync-expo] Android setup complete');
      return config;
    },
  ]);
}

/**
 * Main plugin function - combines iOS and Android plugins
 */
function withSqliteSync(config) {
  console.log('[@sqliteai/sqlite-sync-expo] Configuring SQLite Sync extension...');

  // Apply iOS modifications
  config = withSqliteSyncIOS(config);

  // Apply Android modifications
  config = withSqliteSyncAndroid(config);

  return config;
}

module.exports = withSqliteSync;
`;
}

/**
 * Generate README.md
 */
function generateReadme(version) {
  return `# @sqliteai/sqlite-sync-expo

SQLite Sync extension for Expo apps.

**Version:** ${version}

This package provides pre-built SQLite Sync binaries for iOS and Android, along with an Expo config plugin that automatically configures your native projects.

## Installation

\`\`\`bash
npm install @sqliteai/sqlite-sync-expo @op-engineering/op-sqlite
# or
yarn add @sqliteai/sqlite-sync-expo @op-engineering/op-sqlite
\`\`\`

## Setup

### 1. Add Plugin to app.json

\`\`\`json
{
  "expo": {
    "plugins": ["@sqliteai/sqlite-sync-expo"]
  }
}
\`\`\`

### 2. Run Prebuild

\`\`\`bash
npx expo prebuild --clean
\`\`\`

The plugin will automatically:
- **iOS**: Copy \`CloudSync.xcframework\` and add it to your Xcode project with embed & sign
- **Android**: Copy \`cloudsync.so\` files to \`jniLibs\` for each architecture

### 3. Load Extension in Code

\`\`\`typescript
import { Platform } from 'react-native';
import { getDylibPath, open } from '@op-engineering/op-sqlite';

const db = open({ name: 'mydb.db' });

// Load SQLite Sync extension
if (Platform.OS === 'ios') {
  // iOS requires the bundle ID and framework name
  const path = getDylibPath('ai.sqlite.cloudsync', 'CloudSync');
  db.loadExtension(path);
} else {
  // Android just needs the library name
  db.loadExtension('cloudsync');
}

// Verify it works
const result = db.executeSync('SELECT cloudsync_version() as version');
console.log('SQLite Sync Version:', result.rows[0].version);
\`\`\`

## Supported Platforms

### iOS
- arm64 (devices)
- arm64 + x86_64 simulator

### Android
- arm64-v8a
- armeabi-v7a
- x86_64

## Requirements

- Expo SDK 51+
- React Native 0.73+
- [@op-engineering/op-sqlite](https://www.npmjs.com/package/@op-engineering/op-sqlite) for loading extensions

## Links

- [SQLite Sync Documentation](https://github.com/sqliteai/sqlite-sync)
- [SQLite Cloud](https://sqlitecloud.io)

## License

See [LICENSE.md](./LICENSE.md)
`;
}

/**
 * Main function
 */
function main() {
  const args = process.argv.slice(2);

  if (args.length < 3) {
    console.error('Usage: node generate-expo-package.js <version> <artifacts-dir> <output-dir>');
    console.error('Example: node generate-expo-package.js 0.8.53 ./artifacts ./expo-package');
    process.exit(1);
  }

  const [version, artifactsDir, outputDir] = args;

  // Validate version format
  if (!/^\d+\.\d+\.\d+$/.test(version)) {
    console.error(`Error: Invalid version format: ${version}`);
    console.error('Version must be in semver format (e.g., 0.8.53)');
    process.exit(1);
  }

  // Find LICENSE.md (should be in repo root)
  const licensePath = path.resolve(__dirname, '../../LICENSE.md');
  if (!fs.existsSync(licensePath)) {
    console.error(`Error: LICENSE.md not found at ${licensePath}`);
    process.exit(1);
  }

  console.log(`Generating @sqliteai/sqlite-sync-expo package version ${version}...\n`);

  // Create output directory structure
  const srcDir = path.join(outputDir, 'src');
  const androidDir = path.join(outputDir, 'android', 'jniLibs');
  const iosDir = path.join(outputDir, 'ios');

  fs.mkdirSync(srcDir, { recursive: true });
  fs.mkdirSync(androidDir, { recursive: true });
  fs.mkdirSync(iosDir, { recursive: true });

  // Generate package files
  console.log('Generating package files...');

  // package.json
  fs.writeFileSync(
    path.join(outputDir, 'package.json'),
    JSON.stringify(generatePackageJson(version), null, 2) + '\n'
  );
  console.log('  ✓ package.json');

  // src/index.js
  fs.writeFileSync(path.join(srcDir, 'index.js'), generateIndexJs());
  console.log('  ✓ src/index.js');

  // src/index.d.ts
  fs.writeFileSync(path.join(srcDir, 'index.d.ts'), generateIndexDts());
  console.log('  ✓ src/index.d.ts');

  // app.plugin.js
  fs.writeFileSync(path.join(outputDir, 'app.plugin.js'), generateAppPlugin());
  console.log('  ✓ app.plugin.js');

  // README.md
  fs.writeFileSync(path.join(outputDir, 'README.md'), generateReadme(version));
  console.log('  ✓ README.md');

  // LICENSE.md
  fs.copyFileSync(licensePath, path.join(outputDir, 'LICENSE.md'));
  console.log('  ✓ LICENSE.md');

  // Copy iOS xcframework
  console.log('\nCopying iOS binaries...');
  const xcframeworkSrc = path.join(artifactsDir, 'cloudsync-apple-xcframework', 'CloudSync.xcframework');
  const xcframeworkDest = path.join(iosDir, 'CloudSync.xcframework');

  if (fs.existsSync(xcframeworkSrc)) {
    fs.cpSync(xcframeworkSrc, xcframeworkDest, { recursive: true });
    console.log('  ✓ CloudSync.xcframework');
  } else {
    console.error(`  ✗ CloudSync.xcframework not found at ${xcframeworkSrc}`);
    process.exit(1);
  }

  // Copy Android .so files
  console.log('\nCopying Android binaries...');
  let androidSuccess = 0;

  for (const arch of ANDROID_ARCHS) {
    const soSrc = path.join(artifactsDir, arch.artifact, 'cloudsync.so');
    const archDir = path.join(androidDir, arch.name);
    const soDest = path.join(archDir, 'cloudsync.so');

    if (fs.existsSync(soSrc)) {
      fs.mkdirSync(archDir, { recursive: true });
      fs.copyFileSync(soSrc, soDest);
      console.log(`  ✓ ${arch.name}/cloudsync.so`);
      androidSuccess++;
    } else {
      console.error(`  ✗ ${arch.name}/cloudsync.so not found at ${soSrc}`);
    }
  }

  if (androidSuccess === 0) {
    console.error('\nError: No Android binaries found');
    process.exit(1);
  }

  console.log('\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━');
  console.log(`✅ Generated @sqliteai/sqlite-sync-expo@${version}`);
  console.log(`   iOS: CloudSync.xcframework`);
  console.log(`   Android: ${androidSuccess}/${ANDROID_ARCHS.length} architectures`);
  console.log('━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━');
}

// Run
if (require.main === module) {
  main();
}

module.exports = { generatePackageJson, generateIndexJs, generateAppPlugin, generateReadme };
