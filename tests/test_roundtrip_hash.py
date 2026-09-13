import os
import sys
import subprocess
import hashlib
import tempfile

def sha256_file(filepath):
    h = hashlib.sha256()
    with open(filepath, 'rb') as f:
        while chunk := f.read(8192):
            h.update(chunk)
    return h.hexdigest()

def main():
    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    candidates = [
        os.environ.get("OPENRAR_EXE"),
        os.path.join(root_dir, 'build', 'Release', 'openrar.exe'),
        os.path.join(root_dir, 'build', 'openrar.exe'),
        os.path.join(root_dir, 'build', 'openrar'),
        os.path.join(root_dir, 'build', 'openrar64', 'Release', 'openrar.exe'),
        os.path.join(root_dir, 'build', 'openrar64', 'Debug', 'openrar.exe'),
    ]
    openrar_exe = next((c for c in candidates if c and os.path.exists(c)), None)
        
    if not os.path.exists(openrar_exe):
        print(f"Error: openrar.exe not found at {openrar_exe}")
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmpdir:
        # Create a test file
        test_file = 'test_payload.bin'
        test_filepath = os.path.join(tmpdir, test_file)
        with open(test_filepath, 'wb') as f:
            data = os.urandom(1024) * 4 + b'A_REPEAT_PATTERN' * 1024
            f.write(data * 50)

        orig_hash = sha256_file(test_filepath)
        
        # Compress
        archive_file = 'test.rar'
        archive_filepath = os.path.join(tmpdir, archive_file)
        
        print("Compressing...")
        res_a = subprocess.run([openrar_exe, 'a', archive_filepath, test_file], cwd=tmpdir, capture_output=True, text=True)
        if res_a.returncode != 0:
            print("Compression failed!")
            print(res_a.stdout)
            sys.exit(1)
            
        # Decompress
        extract_dir = os.path.join(tmpdir, 'extracted')
        os.makedirs(extract_dir, exist_ok=True)
        
        print("Decompressing...")
        res_x = subprocess.run([openrar_exe, 'e', archive_filepath, extract_dir + os.sep], cwd=tmpdir, capture_output=True, text=True)
        if res_x.returncode != 0:
            print("Decompression failed!")
            print(res_x.stdout)
            sys.exit(1)
            
        # Compare hash
        extracted_filepath = os.path.join(extract_dir, test_file)
        if not os.path.exists(extracted_filepath):
            print(f"Error: Extracted file not found at {extracted_filepath}")
            sys.exit(1)
            
        new_hash = sha256_file(extracted_filepath)
        
        if orig_hash == new_hash:
            print(f"SUCCESS: Hash roundtrip match! ({orig_hash})")
            sys.exit(0)
        else:
            print(f"ERROR: Hash mismatch!\nOriginal: {orig_hash}\nExtracted: {new_hash}")
            sys.exit(1)

if __name__ == "__main__":
    main()
