const { withDangerousMod, withXcodeProject, withInfoPlist } = require('@expo/config-plugins');
const fs = require('fs');
const path = require('path');
const https = require('https');
const { execSync } = require('child_process');

/**
 * Download file from URL
 */
async function downloadFile(url, dest) {
  return new Promise((resolve, reject) => {
    const file = fs.createWriteStream(dest);
    
    https.get(url, (response) => {
      
      if (response.statusCode === 302 || response.statusCode === 301) {
        file.close();
        fs.unlinkSync(dest);
        // Handle redirect
        return downloadFile(response.headers.location, dest).then(resolve).catch(reject);
      }
      
      if (response.statusCode !== 200) {
        file.close();
        fs.unlinkSync(dest);
        return reject(new Error(`HTTP ${response.statusCode}: ${response.statusMessage}`));
      }
      
      response.pipe(file);
      
      file.on('finish', () => {
        file.close();
        resolve();
      });
      
      file.on('error', (err) => {
        fs.unlink(dest, () => {}); // Delete the file async
        reject(err);
      });
    }).on('error', (err) => {
      reject(err);
    });
  });
}

/**
 * Get latest release URL from GitHub API
 */
async function getLatestReleaseUrl(asset_pattern) {
  return new Promise((resolve, reject) => {
    const options = {
      hostname: 'api.github.com',
      path: '/repos/sqliteai/sqlite-sync/releases/latest',
      headers: {
        'User-Agent': 'expo-cloudsync-plugin'
      }
    };

    https.get(options, (response) => {
      let data = '';
      
      response.on('data', (chunk) => {
        data += chunk;
      });
      
      response.on('end', () => {
        try {
          const release = JSON.parse(data);
          const asset = release.assets.find(asset => 
            asset.name.includes(asset_pattern) && asset.name.endsWith('.zip')
          );
          
          if (asset) {
            resolve(asset.browser_download_url);
          } else {
            reject(new Error(`Asset with pattern ${asset_pattern} not found`));
          }
        } catch (error) {
          reject(error);
        }
      });
    }).on('error', (err) => {
      reject(err);
    });
  });
}

/**
 * Extract zip file and return extracted directory
 */
function extractZip(zipPath, extractTo) {
  try {
    // Create extraction directory
    fs.mkdirSync(extractTo, { recursive: true });
    
    // Use system unzip command
    execSync(`unzip -o "${zipPath}" -d "${extractTo}"`, { stdio: 'pipe' });
    
    return extractTo;
  } catch (error) {
    throw new Error(`Failed to extract ${zipPath}: ${error.message}`);
  }
}

/**
 * Setup Android native libraries
 */
async function setupAndroidLibraries(projectRoot) {
  
  const tempDir = path.join(projectRoot, 'temp_downloads');
  fs.mkdirSync(tempDir, { recursive: true });
  
  try {
    // Download Android x86_64 build
    const x86_64Url = await getLatestReleaseUrl('cloudsync-android-x86_64');
    const x86_64ZipPath = path.join(tempDir, 'android-x86_64.zip');
    await downloadFile(x86_64Url, x86_64ZipPath);
    
    // Download Android arm64 build
    const arm64Url = await getLatestReleaseUrl('cloudsync-android-arm64-v8a');
    const arm64ZipPath = path.join(tempDir, 'android-arm64.zip');
    await downloadFile(arm64Url, arm64ZipPath);
    
    // Extract both archives
    const x86_64ExtractPath = path.join(tempDir, 'x86_64');
    const arm64ExtractPath = path.join(tempDir, 'arm64');
    
    extractZip(x86_64ZipPath, x86_64ExtractPath);
    extractZip(arm64ZipPath, arm64ExtractPath);
    
    // Setup jniLibs directory structure
    const jniLibsDir = path.join(projectRoot, 'android', 'app', 'src', 'main', 'jniLibs');
    const x86_64LibDir = path.join(jniLibsDir, 'x86_64');
    const arm64LibDir = path.join(jniLibsDir, 'arm64-v8a');
    
    fs.mkdirSync(x86_64LibDir, { recursive: true });
    fs.mkdirSync(arm64LibDir, { recursive: true });
    
    // Find and copy cloudsync.so files
    const findSoFile = (dir) => {
      const files = fs.readdirSync(dir, { recursive: true });
      const soFile = files.find(file => file.toString().endsWith('cloudsync.so'));
      return soFile ? path.join(dir, soFile) : null;
    };
    
    const x86_64SoFile = findSoFile(x86_64ExtractPath);
    const arm64SoFile = findSoFile(arm64ExtractPath);
    
    if (x86_64SoFile) {
      fs.copyFileSync(x86_64SoFile, path.join(x86_64LibDir, 'cloudsync.so'));
    }
    
    if (arm64SoFile) {
      fs.copyFileSync(arm64SoFile, path.join(arm64LibDir, 'cloudsync.so'));
    }
    
  } finally {
    // Cleanup temp directory
    if (fs.existsSync(tempDir)) {
      fs.rmSync(tempDir, { recursive: true, force: true });
    }
  }
}

/**
 * Setup iOS framework
 */
async function setupIOSFramework(projectRoot) {
  
  const tempDir = path.join(projectRoot, 'temp_downloads');
  fs.mkdirSync(tempDir, { recursive: true });
  
  try {
    // Download iOS xcframework
    const xcframeworkUrl = await getLatestReleaseUrl('cloudsync-apple-xcframework');
    const xcframeworkZipPath = path.join(tempDir, 'apple-xcframework.zip');
    await downloadFile(xcframeworkUrl, xcframeworkZipPath);
    
    // Extract xcframework
    const extractPath = path.join(tempDir, 'xcframework');
    extractZip(xcframeworkZipPath, extractPath);
    
    // Find CloudSync.xcframework directory
    const findXcframework = (dir) => {
      const files = fs.readdirSync(dir, { recursive: true });
      const xcframeworkPath = files.find(file => file.toString().endsWith('CloudSync.xcframework'));
      return xcframeworkPath ? path.join(dir, xcframeworkPath) : null;
    };
    
    const xcframeworkPath = findXcframework(extractPath);
    
    if (xcframeworkPath && fs.statSync(xcframeworkPath).isDirectory()) {
      // Get project name from app.json
      const appJson = JSON.parse(fs.readFileSync(path.join(projectRoot, 'app.json'), 'utf8'));
      const projectName = appJson.expo.name;
      
      const frameworksDir = path.join(projectRoot, 'ios', projectName, 'Frameworks');
      const targetFrameworkPath = path.join(frameworksDir, 'CloudSync.xcframework');
      
      // Create Frameworks directory
      fs.mkdirSync(frameworksDir, { recursive: true });
      
      // Copy xcframework
      fs.cpSync(xcframeworkPath, targetFrameworkPath, { recursive: true });
      
      return targetFrameworkPath;
    } else {
      throw new Error('CloudSync.xcframework not found in extracted archive');
    }
    
  } finally {
    // Cleanup temp directory
    if (fs.existsSync(tempDir)) {
      fs.rmSync(tempDir, { recursive: true, force: true });
    }
  }
}

/**
 * Add framework to Xcode project
 */
const withCloudSyncFramework = (config) => {
  return withXcodeProject(config, (config) => {
    const xcodeProject = config.modResults;
    const projectName = config.modRequest.projectName || config.name;
    const target = xcodeProject.getFirstTarget().uuid;

    const frameworkPath = `${projectName}/Frameworks/CloudSync.xcframework`;
    
    // Check if framework already exists
    if (xcodeProject.hasFile(frameworkPath)) {
      return config;
    }
    
    // First check if "Embed Frameworks" build phase exists, if not create it
    let embedPhase = xcodeProject.pbxEmbedFrameworksBuildPhaseObj(target);
    
    if (!embedPhase) {
      // Create the embed frameworks build phase with correct parameters for frameworks
      xcodeProject.addBuildPhase([], 'PBXCopyFilesBuildPhase', 'Embed Frameworks', target, 'frameworks');
      embedPhase = xcodeProject.pbxEmbedFrameworksBuildPhaseObj(target);
    }
    
    // Add framework to project with embed
    const frameworkFile = xcodeProject.addFramework(frameworkPath, {
      target: target,
      embed: true,
      customFramework: true
    });
    
    
    return config;
  });
};

/**
 * Main plugin function
 */
const withCloudSync = (config) => {
  // Android setup
  config = withDangerousMod(config, [
    'android',
    async (config) => {
      await setupAndroidLibraries(config.modRequest.projectRoot);
      return config;
    }
  ]);
  
  // iOS setup - download and place framework
  config = withDangerousMod(config, [
    'ios',
    async (config) => {
      await setupIOSFramework(config.modRequest.projectRoot);
      return config;
    }
  ]);
  
  // iOS setup - add to Xcode project
  config = withCloudSyncFramework(config);

  // iOS setup - register icon fonts in Info.plist
  config = withInfoPlist(config, (config) => {
    const fonts = config.modResults.UIAppFonts || [];
    if (!fonts.includes('FontAwesome.ttf')) {
      fonts.push('FontAwesome.ttf');
    }
    if (!fonts.includes('MaterialDesignIcons.ttf')) {
      fonts.push('MaterialDesignIcons.ttf');
    }
    config.modResults.UIAppFonts = fonts;
    return config;
  });

  return config;
};

module.exports = withCloudSync;
