#!/bin/bash
set -e

EMDIR="${EMDIR:-/emsdk/upstream/emscripten}"
SRCDIR="${SRCDIR:-/src}"
BUILDDIR="${BUILDDIR:-/build}"

echo "emscripten: $EMDIR"
echo "source:     $SRCDIR"
echo "build:      $BUILDDIR"

# 1. Add DIRECT_SOCKETS setting to settings.js (only if not already present)
if ! grep -q 'var DIRECT_SOCKETS' "$EMDIR/src/settings.js"; then
  sed -i '/^var PROXY_POSIX_SOCKETS = false;/a\
var DIRECT_SOCKETS = false;' "$EMDIR/src/settings.js"
  echo "Patched settings.js"
fi

# 2. Guard default socket syscalls in libsyscall.js
if ! grep -q 'DIRECT_SOCKETS' "$EMDIR/src/lib/libsyscall.js"; then
  sed -i 's/^#if PROXY_POSIX_SOCKETS == 0 && WASMFS == 0$/#if PROXY_POSIX_SOCKETS == 0 \&\& WASMFS == 0 \&\& DIRECT_SOCKETS == 0/' \
    "$EMDIR/src/lib/libsyscall.js"
  echo "Patched libsyscall.js guard"
fi

# 3. Patch fd_close in libwasi.js - add DIRECT_SOCKETS handler after PROXY_POSIX_SOCKETS
if ! grep -q 'DIRECT_SOCKETS' "$EMDIR/src/lib/libwasi.js"; then
python3 -c "
path = '$EMDIR/src/lib/libwasi.js'
with open(path) as f:
    content = f.read()

old_block = '''#elif PROXY_POSIX_SOCKETS
    // close() is a tricky function because it can be used to close both regular file descriptors
    // and POSIX network socket handles, hence an implementation would need to track for each
    // file descriptor which kind of item it is. To simplify, when using PROXY_POSIX_SOCKETS
    // option, use shutdown() to close a socket, and this function should behave like a no-op.
    warnOnce('To close sockets with PROXY_POSIX_SOCKETS bridge, prefer to use the function shutdown() that is proxied, instead of close()')
    return 0;'''

new_block = old_block + '''
#elif DIRECT_SOCKETS
    var sock = DIRECT_SOCKETS.getSocket(fd);
    if (sock) {
      DIRECT_SOCKETS._closeSocket(sock);
      delete DIRECT_SOCKETS.sockets[fd];
      return 0;
    }
    return 0;'''

content = content.replace(old_block, new_block)
with open(path, 'w') as f:
    f.write(content)
print('Patched libwasi.js')
"
fi

# 4. Copy our new library file
cp "$SRCDIR/emscripten/src/lib/libdirectsockets.js" "$EMDIR/src/lib/libdirectsockets.js"
echo "Copied libdirectsockets.js"

# 5. Register libdirectsockets.js in modules.mjs so the JS compiler loads it
if ! grep -q 'libdirectsockets' "$EMDIR/src/modules.mjs"; then
python3 -c "
path = '$EMDIR/src/modules.mjs'
with open(path) as f:
    content = f.read()

old = '''  if (!WASMFS) {
    libraries.push('libsyscall.js');
  }'''

new = '''  if (!WASMFS) {
    libraries.push('libsyscall.js');
  }

  if (DIRECT_SOCKETS) {
    libraries.push('libdirectsockets.js');
  }'''

content = content.replace(old, new)
with open(path, 'w') as f:
    f.write(content)
print('Patched modules.mjs')
"
fi

# 6. Patch syscall stubs to remove C stubs that conflict with our JS implementations
STUBS_FILE="$EMDIR/system/lib/libc/emscripten_syscall_stubs.c"
if [ -f "$STUBS_FILE" ]; then
python3 -c "
import re
path = '$STUBS_FILE'
with open(path) as f:
    content = f.read()

changed = False

# Remove __syscall_setsockopt stub
old_setsockopt = '''weak int __syscall_setsockopt(int sockfd, int level, int optname, intptr_t optval, size_t optlen, int dummy) {
  REPORT(setsockopt);
  return -ENOPROTOOPT; // The option is unknown at the level indicated.
}'''
if old_setsockopt in content:
    content = content.replace(old_setsockopt,
        '// Removed: __syscall_setsockopt - provided by libdirectsockets.js when DIRECT_SOCKETS is enabled')
    changed = True

# Remove __syscall_poll stub (we provide async JS implementation)
poll_pattern = r'weak int __syscall_poll\([^)]*\)\s*\{[^}]*REPORT\(poll\)[^}]*\}'
m = re.search(poll_pattern, content)
if m:
    content = content.replace(m.group(0),
        '// Removed: __syscall_poll - provided by libdirectsockets.js when DIRECT_SOCKETS is enabled')
    changed = True

# Remove __syscall_pselect6 stub (select converts to poll in musl)
pselect_pattern = r'weak int __syscall_pselect6\([^)]*\)\s*\{[^}]*REPORT\(pselect6\)[^}]*\}'
m = re.search(pselect_pattern, content)
if m:
    content = content.replace(m.group(0),
        '// Removed: __syscall_pselect6 - select/pselect handled via poll in libdirectsockets.js')
    changed = True

# Remove __syscall_pipe2 stub (we provide in-memory pipe implementation)
pipe2_pattern = r'weak int __syscall_pipe2\([^)]*\)\s*\{[^}]*REPORT\(pipe2\)[^}]*\}'
m = re.search(pipe2_pattern, content)
if m:
    content = content.replace(m.group(0),
        '// Removed: __syscall_pipe2 - provided by libdirectsockets.js when DIRECT_SOCKETS is enabled')
    changed = True

# Remove __syscall_fcntl64 stub (we provide O_NONBLOCK support)
fcntl_pattern = r'weak int __syscall_fcntl64\([^)]*\)\s*\{[^}]*REPORT\(fcntl64\)[^}]*\}'
m = re.search(fcntl_pattern, content)
if m:
    content = content.replace(m.group(0),
        '// Removed: __syscall_fcntl64 - provided by libdirectsockets.js when DIRECT_SOCKETS is enabled')
    changed = True

# Remove __syscall_ioctl stub (we provide FIONBIO/FIONREAD)
ioctl_pattern = r'weak int __syscall_ioctl\([^)]*\)\s*\{[^}]*REPORT\(ioctl\)[^}]*\}'
m = re.search(ioctl_pattern, content)
if m:
    content = content.replace(m.group(0),
        '// Removed: __syscall_ioctl - provided by libdirectsockets.js when DIRECT_SOCKETS is enabled')
    changed = True

# Remove __syscall_socketpair stub
socketpair_pattern = r'weak int __syscall_socketpair\([^)]*\)\s*\{[^}]*REPORT\(socketpair\)[^}]*\}'
m = re.search(socketpair_pattern, content)
if m:
    content = content.replace(m.group(0),
        '// Removed: __syscall_socketpair - provided by libdirectsockets.js when DIRECT_SOCKETS is enabled')
    changed = True

if changed:
    with open(path, 'w') as f:
        f.write(content)
    print('Patched emscripten_syscall_stubs.c (removed poll/pipe2/fcntl64/ioctl/socketpair stubs)')
else:
    print('emscripten_syscall_stubs.c: no matching stubs found (may already be patched)')
"
fi

# 6b. Patch fd_close in libwasi.js to also handle pipe fd cleanup
if ! grep -q 'DIRECT_SOCKETS_PIPES' "$EMDIR/src/lib/libwasi.js"; then
python3 -c "
path = '$EMDIR/src/lib/libwasi.js'
with open(path) as f:
    content = f.read()

# Add pipe cleanup to the DIRECT_SOCKETS close handler
old_ds_close = '''#elif DIRECT_SOCKETS
    var sock = DIRECT_SOCKETS.getSocket(fd);
    if (sock) {
      DIRECT_SOCKETS._closeSocket(sock);
      delete DIRECT_SOCKETS.sockets[fd];
      return 0;
    }
    return 0;'''

new_ds_close = '''#elif DIRECT_SOCKETS
    var sock = DIRECT_SOCKETS.getSocket(fd);
    if (sock) {
      DIRECT_SOCKETS._closeSocket(sock);
      delete DIRECT_SOCKETS.sockets[fd];
      return 0;
    }
    if (typeof DIRECT_SOCKETS_PIPES !== 'undefined' && DIRECT_SOCKETS_PIPES.closePipeFd(fd)) {
      return 0;
    }
    return 0;'''

if old_ds_close in content:
    content = content.replace(old_ds_close, new_ds_close)
    with open(path, 'w') as f:
        f.write(content)
    print('Patched libwasi.js (added pipe fd cleanup)')
else:
    print('libwasi.js: DIRECT_SOCKETS_PIPES patch not needed or already applied')
"
fi

# 6c. Patch fd_read and fd_write in libwasi.js to handle pipe/socketpair fds
# read()/write() on pipe fds go through WASI fd_read/fd_write, not socket syscalls.
# The WASI function signatures vary by emscripten version, so we extract param names dynamically.
if ! grep -q 'DIRECT_SOCKETS_PIPES.*readPipe' "$EMDIR/src/lib/libwasi.js"; then
python3 -c "
import re
path = '$EMDIR/src/lib/libwasi.js'
with open(path) as f:
    content = f.read()

# Patch fd_read: extract actual parameter names and inject pipe check
fd_read_pattern = r'(fd_read\s*:\s*(?:async\s+)?function\s*\(([^)]*)\)\s*\{)'
m = re.search(fd_read_pattern, content)
if m:
    inject_after = m.group(0)
    params = [p.strip() for p in m.group(2).split(',')]
    # params should be like [fd, iov, iovcnt, pnum] but names vary
    p_fd = params[0] if len(params) > 0 else 'fd'
    p_iov = params[1] if len(params) > 1 else 'iov'
    p_iovcnt = params[2] if len(params) > 2 else 'iovcnt'
    p_pnum = params[3] if len(params) > 3 else 'pnum'

    pipe_read_check = '''
#if DIRECT_SOCKETS
    if (typeof DIRECT_SOCKETS_PIPES !== 'undefined') {
      var __pipeEntry = DIRECT_SOCKETS_PIPES.getPipe(''' + p_fd + ''');
      if (__pipeEntry) {
        var __totalRead = 0;
        for (var __pi = 0; __pi < ''' + p_iovcnt + '''; __pi++) {
          var __ptr = {{{ makeGetValue(\"''' + p_iov + '''\", '__pi * 8', POINTER_TYPE) }}};
          var __len = {{{ makeGetValue(\"''' + p_iov + '''\", '__pi * 8 + 4', POINTER_TYPE) }}};
          var __chunk = DIRECT_SOCKETS_PIPES.readPipe(''' + p_fd + ''', __len);
          if (__chunk && __chunk.length > 0) {
            HEAPU8.set(__chunk, __ptr);
            __totalRead += __chunk.length;
          }
        }
        {{{ makeSetValue(\"''' + p_pnum + '''\", 0, '__totalRead', POINTER_TYPE) }}};
        return 0;
      }
    }
#endif
'''
    content = content.replace(inject_after, inject_after + pipe_read_check, 1)
    print('Patched fd_read for pipe support (params: ' + ', '.join(params) + ')')
else:
    print('WARNING: Could not find fd_read function in libwasi.js')

# Patch fd_write: extract actual parameter names and inject pipe check
fd_write_pattern = r'(fd_write\s*:\s*(?:async\s+)?function\s*\(([^)]*)\)\s*\{)'
m = re.search(fd_write_pattern, content)
if m:
    inject_after = m.group(0)
    params = [p.strip() for p in m.group(2).split(',')]
    p_fd = params[0] if len(params) > 0 else 'fd'
    p_iov = params[1] if len(params) > 1 else 'iov'
    p_iovcnt = params[2] if len(params) > 2 else 'iovcnt'
    p_pnum = params[3] if len(params) > 3 else 'pnum'

    pipe_write_check = '''
#if DIRECT_SOCKETS
    if (typeof DIRECT_SOCKETS_PIPES !== 'undefined') {
      var __pipeEntry = DIRECT_SOCKETS_PIPES.getPipe(''' + p_fd + ''');
      if (__pipeEntry) {
        var __totalWritten = 0;
        for (var __pi = 0; __pi < ''' + p_iovcnt + '''; __pi++) {
          var __ptr = {{{ makeGetValue(\"''' + p_iov + '''\", '__pi * 8', POINTER_TYPE) }}};
          var __len = {{{ makeGetValue(\"''' + p_iov + '''\", '__pi * 8 + 4', POINTER_TYPE) }}};
          var __data = HEAPU8.slice(__ptr, __ptr + __len);
          var __rc = DIRECT_SOCKETS_PIPES.writePipe(''' + p_fd + ''', __data);
          if (__rc < 0) {
            if (__totalWritten > 0) break;
            {{{ makeSetValue(\"''' + p_pnum + '''\", 0, '0', POINTER_TYPE) }}};
            return 29; /* __WASI_ERRNO_PIPE */
          }
          __totalWritten += __rc;
        }
        {{{ makeSetValue(\"''' + p_pnum + '''\", 0, '__totalWritten', POINTER_TYPE) }}};
        return 0;
      }
    }
#endif
'''
    content = content.replace(inject_after, inject_after + pipe_write_check, 1)
    print('Patched fd_write for pipe support (params: ' + ', '.join(params) + ')')
else:
    print('WARNING: Could not find fd_write function in libwasi.js')

with open(path, 'w') as f:
    f.write(content)
"
fi

# 6d. Patch _emscripten_lookup_name in emscripten to allow our async DoH override
# Our libdirectsockets.js provides an async _emscripten_lookup_name that does real DNS via DoH.
# We need to remove or guard the original sync definition to avoid duplicate symbol conflicts.
for LOOKUPFILE in "$EMDIR/src/lib/libsockfs.js" "$EMDIR/src/lib/libsyscall.js" "$EMDIR/src/lib/libnetworking.js"; do
  if [ -f "$LOOKUPFILE" ] && grep -q '_emscripten_lookup_name' "$LOOKUPFILE" 2>/dev/null; then
    if ! grep -q 'DIRECT_SOCKETS.*_emscripten_lookup_name' "$LOOKUPFILE" 2>/dev/null; then
python3 -c "
import re
path = '$LOOKUPFILE'
with open(path) as f:
    content = f.read()

# Wrap the _emscripten_lookup_name definition in a !DIRECT_SOCKETS guard
# This handles various possible formats of the definition
pattern = r'(_emscripten_lookup_name\s*(?::|\s*=))'
m = re.search(pattern, content)
if m:
    # Find the start of the block (including __deps, __async annotations)
    pos = m.start()
    # Look backwards for any annotation lines (deps, sig, etc)
    lines = content[:pos].split('\n')
    # Check preceding lines for related annotations
    start = pos
    for j in range(len(lines)-1, max(len(lines)-5, -1), -1):
        line = lines[j].strip()
        if '_emscripten_lookup_name__' in line:
            # Calculate character position of this line
            start = len('\n'.join(lines[:j]))
            if start > 0: start += 1
        else:
            break

    # Insert a preprocessor guard before the function definition
    guard_start = '\n#if !DIRECT_SOCKETS\n'

    # Find the end of the function definition
    # Look for the next top-level entry (line starting without indentation containing ':' or end of object)
    rest = content[m.end():]
    # Simple heuristic: find matching braces
    brace_depth = 0
    found_opening = False
    end_offset = 0
    for ci, ch in enumerate(rest):
        if ch == '{':
            brace_depth += 1
            found_opening = True
        elif ch == '}':
            brace_depth -= 1
            if found_opening and brace_depth == 0:
                end_offset = m.end() + ci + 1
                break

    if end_offset > 0:
        # Find the next comma or end
        tail = content[end_offset:]
        comma_pos = 0
        for ci, ch in enumerate(tail):
            if ch == ',':
                comma_pos = ci + 1
                break
            elif not ch.isspace() and ch != '\n':
                break

        guard_end_pos = end_offset + comma_pos
        guard_end = '\n#endif // !DIRECT_SOCKETS\n'

        content = content[:start] + guard_start + content[start:guard_end_pos] + guard_end + content[guard_end_pos:]

        with open(path, 'w') as f:
            f.write(content)
        print('Patched ' + path + ': guarded _emscripten_lookup_name with !DIRECT_SOCKETS')
    else:
        print('Could not find end of _emscripten_lookup_name in ' + path)
else:
    print('No _emscripten_lookup_name found in ' + path)
"
    fi
  fi
done

# 7. Clear emscripten cache to pick up JS changes
rm -rf "$EMDIR/cache"
echo "Cleared cache"

echo ""
echo "=== Compiling test_direct_sockets.cpp ==="
echo ""

em++ "$SRCDIR/test_direct_sockets.cpp" -o "$BUILDDIR/test.js" \
  -sDIRECT_SOCKETS \
  -sJSPI \
  -sPROXY_TO_PTHREAD \
  -pthread \
  -sEXIT_RUNTIME=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  2>&1

echo ""
echo "=== Build complete ==="
ls -la "$BUILDDIR/test."*
