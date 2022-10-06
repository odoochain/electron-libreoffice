#!/bin/bash

set -e
SCRIPT_DIR=$(cd -- "$(dirname $(dirname -- "${BASH_SOURCE[0]}"))" &> /dev/null && pwd)
BASE_PATH="$(dirname -- "$SCRIPT_DIR")"
DEPOT_TOOLS_PATH="$BASE_PATH/depot_tools"
export PATH="$DEPOT_TOOLS_PATH:$SCRIPT_DIR:$PATH"

scripts/cross-sync/reset-after.sh

# It's not possible to override this as command line arg
(cd "$BASE_PATH"; patch <<'EOF'
diff --git a/.gclient b/.gclient
index 5b21302..c2b4a5d 100644
--- a/.gclient
+++ b/.gclient
@@ -11,6 +11,7 @@ solutions = [
       "src/third_party/android_rust_toolchain": None,
       "src/chrome/test/data/xr/webvr_info": None,
     },
-    "custom_vars": {},
+    "custom_vars": {"host_os":"mac", "checkout_mac": True, "checkout_linux": False, "target_os": "mac", "target_cpu": "x64"},
   },
 ]
+target_os = ['mac']
EOF
)

# CIPD doesn't allow overriding the platform variable
(cd "$DEPOT_TOOLS_PATH"; patch <<'EOF'
diff --git a/gclient.py b/gclient.py
index 3b7dc4b..628f61b 100755
--- a/gclient.py
+++ b/gclient.py
@@ -2200,7 +2200,7 @@ class CipdDependency(Dependency):
   def __init__(
       self, parent, name, dep_value, cipd_root,
       custom_vars, should_process, relative, condition):
-    package = dep_value['package']
+    package = dep_value['package'].replace('${platform}', 'mac-amd64')
     version = dep_value['version']
     url = urlparse.urljoin(
         cipd_root.service_url, '%s@%s' % (package, version))
EOF
)

# Prevent depot-tools from self-updating and erasing any changes during sync
export DEPOT_TOOLS_UPDATE=0
(cd "$BASE_PATH"; gclient sync -R -D --no-history --with_branch_heads --with_tags -v -v --deps=mac)
(cd "$BASE_PATH"; tar --zstd -cf mac.tzstd --anchored --exclude-from="$SCRIPT_DIR/tar_excludes_os.txt" --exclude-from="$SCRIPT_DIR/tar_thirdparty_includes.txt" --files-from="$SCRIPT_DIR/tar_includes_os.txt")
