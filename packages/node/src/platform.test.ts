import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { arch, platform } from 'node:os';
import { existsSync } from 'node:fs';
import { getCurrentPlatform, getPlatformPackageName } from './platform';

vi.mock('node:os', () => ({ arch: vi.fn(), platform: vi.fn() }));
vi.mock('node:fs', () => ({ existsSync: vi.fn(), readFileSync: vi.fn() }));
vi.mock('node:child_process', () => ({ execSync: vi.fn(() => 'glibc') }));

describe('native binary architecture selection', () => {
  beforeEach(() => {
    vi.mocked(existsSync).mockReturnValue(false);
    vi.spyOn(process.report, 'getReport').mockReturnValue({
      header: { glibcVersionRuntime: '2.38' },
    } as ReturnType<typeof process.report.getReport>);
  });
  afterEach(() => vi.restoreAllMocks());
  it.each(['darwin', 'linux', 'win32'] as const)('rejects ia32 on %s', (os) => {
    vi.mocked(platform).mockReturnValue(os);
    vi.mocked(arch).mockReturnValue('ia32');
    expect(() => getCurrentPlatform()).toThrow(`Unsupported platform: ${os}-ia32`);
  });
  it.each(['darwin', 'linux', 'win32'] as const)('keeps x64 support on %s', (os) => {
    vi.mocked(platform).mockReturnValue(os);
    vi.mocked(arch).mockReturnValue('x64');
    expect(getPlatformPackageName()).toBe(`@sqliteai/sqlite-sync-${os}-x86_64`);
  });
  it('keeps Linux arm64 musl support', () => {
    vi.mocked(platform).mockReturnValue('linux');
    vi.mocked(arch).mockReturnValue('arm64');
    vi.mocked(existsSync).mockReturnValue(true);
    expect(getCurrentPlatform()).toBe('linux-arm64-musl');
  });
});
