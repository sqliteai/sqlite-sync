// swift-tools-version: 6.1
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "CloudSync",
    platforms: [.macOS(.v11), .iOS(.v11), .macCatalyst(.v14)],
    products: [
        .library(
            name: "CloudSync",
            targets: ["CloudSync"])
    ],
    targets: [
        .binaryTarget(
            name: "CloudSyncBinary",
            url: "https://github.com/sqliteai/sqlite-sync/releases/download/1.2.3/cloudsync-apple-xcframework-1.2.3.zip",
            checksum: "a7cef7201f240596639e444475a7a905a46fe4706104d85ef1e866d4f0851d6a"
        ),
        .target(
            name: "CloudSync",
            dependencies: ["CloudSyncBinary"],
            path: "packages/swift"
        ),
    ]
)
