// swift-tools-version: 6.1
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "CloudSync",
    platforms: [.macOS(.v11), .iOS(.v11)],
    products: [
        .library(
            name: "CloudSync",
            targets: ["CloudSync"])
    ],
    targets: [
        .binaryTarget(
            name: "CloudSyncBinary",
            url: "https://github.com/sqliteai/sqlite-sync/releases/download/1.0.15/cloudsync-apple-xcframework-1.0.15.zip",
            checksum: "0a3422cc1357563c22d682ebf823b5ebcceab793158966364d3ce1be4f4ddf87"
        ),
        .target(
            name: "CloudSync",
            dependencies: ["CloudSyncBinary"],
            path: "packages/swift"
        ),
    ]
)
