from flask import Flask, render_template, request, jsonify, send_file, send_from_directory
import subprocess
import os
import tempfile
import json
import shutil
import logging
import traceback
from datetime import datetime
import sys

app = Flask(__name__)
app.config['MAX_CONTENT_LENGTH'] = 16 * 1024 * 1024  # 16MB 文件大小限制

# 配置日志系统
if not os.path.exists('logs'):
    os.makedirs('logs')

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler(f'logs/judge_system_{datetime.now().strftime("%Y%m%d")}.log'),
        logging.StreamHandler(sys.stdout)
    ]
)
logger = logging.getLogger(__name__)

# 路径配置（相对于 app.py 的位置）
FRONTEND_DIR = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.dirname(FRONTEND_DIR)
BACKEND_DIR = os.path.join(SRC_DIR, 'backend')
PROJECT_ROOT = os.path.dirname(SRC_DIR)
INCLUDE_DIR = os.path.join(PROJECT_ROOT, 'include')

# 新增：数据保存目录
DATA_DIR = os.path.join(FRONTEND_DIR, 'data')
os.makedirs(DATA_DIR, exist_ok=True)
logger.info(f"Data directory: {DATA_DIR}")

# C++ 可执行文件路径（Windows 下为 .exe）
JUDGE_EXE = os.path.join(BACKEND_DIR, 'judge_parallel.exe')

# 如果 Windows 下找不到 .exe，尝试无后缀版本（Linux/Mac 兼容）
if not os.path.exists(JUDGE_EXE):
    JUDGE_EXE = os.path.join(BACKEND_DIR, 'judge_parallel')

def check_backend():
    """检查后端可执行文件是否存在"""
    if not os.path.exists(JUDGE_EXE):
        return False, f"Backend not found: {JUDGE_EXE}"
    return True, "OK"

@app.route('/')
def index():
    """渲染主页"""
    backend_ok, msg = check_backend()
    if not backend_ok:
        return f"""
        <h1>Configuration Error</h1>
        <p>{msg}</p>
        <p>Please compile the C++ backend first:</p>
        <pre>cd src/backend
g++ -O2 -std=c++17 -I ../../include -pthread -o judge_parallel judge_parallel.cpp</pre>
        """, 500
    return render_template('index.html')

@app.route('/judge', methods=['POST'])
def judge():
    """处理评测请求"""
    request_id = datetime.now().strftime("%Y%m%d_%H%M%S_") + os.urandom(4).hex()
    logger.info(f"[{request_id}] Judge request received")
    
    # 检查后端
    backend_ok, msg = check_backend()
    if not backend_ok:
        logger.error(f"[{request_id}] Backend check failed: {msg}")
        return jsonify({'error': msg}), 500
    
    # 检查文件上传
    required_files = ['make', 'ans', 'unknown']
    for name in required_files:
        if name not in request.files:
            logger.error(f"[{request_id}] Missing file: {name}.cpp")
            return jsonify({'error': f'Missing file: {name}.cpp'}), 400
    
    # 获取参数 k
    try:
        k = int(request.form.get('k', '1'))
        if k < 1 or k >= 50:
            logger.error(f"[{request_id}] Invalid k value: {k}")
            return jsonify({'error': 'k must be between 1 and 49'}), 400
    except ValueError:
        logger.error(f"[{request_id}] Invalid k value format")
        return jsonify({'error': 'Invalid k value'}), 400
    
    # 获取语言标准参数
    std_version = request.form.get('std_version', 'c++17').lower()
    supported_stds = ['c++98', 'c++11', 'c++14', 'c++17', 'c++20']
    if std_version not in supported_stds:
        logger.error(f"[{request_id}] Unsupported C++ standard: {std_version}")
        return jsonify({'error': f'Unsupported C++ standard: {std_version}'}), 400
    
    # 获取时间和内存限制
    try:
        time_limit_ms = int(request.form.get('time_limit', '2000'))
        if time_limit_ms < 1 or time_limit_ms > 10000:
            logger.error(f"[{request_id}] Invalid time limit: {time_limit_ms}")
            return jsonify({'error': 'Time limit must be between 1 and 10000 ms'}), 400
    except ValueError:
        logger.error(f"[{request_id}] Invalid time limit format")
        return jsonify({'error': 'Invalid time limit'}), 400
    
    try:
        memory_limit_mb = int(request.form.get('memory_limit', '512'))
        if memory_limit_mb < 1 or memory_limit_mb > 1024:
            logger.error(f"[{request_id}] Invalid memory limit: {memory_limit_mb}")
            return jsonify({'error': 'Memory limit must be between 1 and 1024 MB'}), 400
    except ValueError:
        logger.error(f"[{request_id}] Invalid memory limit format")
        return jsonify({'error': 'Invalid memory limit'}), 400
    
    logger.info(f"[{request_id}] Parameters: k={k}, std={std_version}, time={time_limit_ms}ms, memory={memory_limit_mb}MB")
    
    # 创建临时目录
    temp_dir = tempfile.mkdtemp(prefix=f'judge_{request_id}_')
    file_paths = {}
    
    try:
        # 保存上传的文件
        for name in required_files:
            file = request.files[name]
            if file.filename == '':
                logger.error(f"[{request_id}] Empty file: {name}")
                return jsonify({'error': f'Empty file: {name}'}), 400
            
            save_path = os.path.join(temp_dir, f'{name}.cpp')
            file.save(save_path)
            file_paths[name] = save_path
            logger.info(f"[{request_id}] Saved {name}.cpp: {file.filename}")
        
        # 准备命令行参数（现在有 8 个参数，包含保存目录）
        cmd = [
            JUDGE_EXE,
            file_paths['make'],
            file_paths['ans'],
            file_paths['unknown'],
            str(k),
            std_version,
            str(time_limit_ms),
            str(memory_limit_mb),
            DATA_DIR  # 第9个参数：错误文件保存目录
        ]
        
        logger.info(f"[{request_id}] Executing command: {' '.join(cmd[:8])} [DATA_DIR]")
        
        # 执行评测
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=300,
            shell=False,
            cwd=temp_dir,
            encoding='utf-8',
            errors='ignore'
        )
        
        logger.info(f"[{request_id}] Process completed with returncode: {result.returncode}")
        
        if result.returncode != 0:
            error_msg = f'Judge execution failed (code: {result.returncode})'
            if result.stderr:
                error_msg += f': {result.stderr[:200]}'
            logger.error(f"[{request_id}] {error_msg}")
            return jsonify({
                'error': error_msg,
                'stderr': result.stderr[:500] if result.stderr else '',
                'stdout': result.stdout[:500] if result.stdout else '',
                'returncode': result.returncode
            }), 500
        
        try:
            output = json.loads(result.stdout)
            logger.info(f"[{request_id}] Judge completed: AC={output.get('ac', 0)}, "
                       f"WA={output.get('wa', 0)}, RE={output.get('re', 0)}, "
                       f"TLE={output.get('tle', 0)}, MLE={output.get('mle', 0)}, "
                       f"CE={output.get('ce', 0)}, UKE={output.get('uke', 0)}")
            
            # 记录哪些任务保存了错误文件
            saved_tasks = [r for r in output.get('results', []) if r.get('files_saved')]
            if saved_tasks:
                logger.info(f"[{request_id}] Saved error files for tasks: {[r['id'] for r in saved_tasks]}")
            
            # 保存结果到日志文件
            result_log_path = os.path.join(DATA_DIR, f'judge_result_{request_id}.json')
            with open(result_log_path, 'w', encoding='utf-8') as f:
                json.dump(output, f, ensure_ascii=False, indent=2)
            
            return jsonify(output)
        except json.JSONDecodeError as e:
            logger.error(f"[{request_id}] JSON decode error: {str(e)}")
            logger.error(f"[{request_id}] Raw output: {result.stdout[:500]}")
            return jsonify({
                'error': 'Invalid JSON output from judge',
                'raw_output': result.stdout[:500],
                'parse_error': str(e)
            }), 500
            
    except subprocess.TimeoutExpired:
        logger.error(f"[{request_id}] Judge timeout (>5 minutes)")
        return jsonify({'error': 'Judge timeout (>5 minutes)'}), 504
    except Exception as e:
        logger.error(f"[{request_id}] Unexpected error: {str(e)}")
        logger.error(traceback.format_exc())
        return jsonify({'error': str(e)}), 500
    finally:
        # 清理临时目录
        if os.path.exists(temp_dir):
            shutil.rmtree(temp_dir, ignore_errors=True)
            logger.info(f"[{request_id}] Cleaned temp directory: {temp_dir}")

# 新增：下载错误文件的接口
@app.route('/download/<int:task_id>/<file_type>')
def download_error_file(task_id, file_type):
    """下载指定任务的错误文件
    file_type: input, expected, output, log, summary
    """
    valid_types = {
        'input': 'input.txt',
        'expected': 'expected.txt',
        'output': 'output.txt',
        'log': 'log.txt',
        'summary': 'summary.txt'
    }
    
    if file_type not in valid_types:
        return jsonify({'error': f'Invalid file type. Valid types: {list(valid_types.keys())}'}), 400
    
    task_dir = os.path.join(DATA_DIR, f'task_{task_id}')
    file_path = os.path.join(task_dir, valid_types[file_type])
    
    if not os.path.exists(file_path):
        return jsonify({'error': 'File not found'}), 404
    
    try:
        return send_file(file_path, as_attachment=True, download_name=f'task_{task_id}_{file_type}.txt')
    except Exception as e:
        logger.error(f"Download error: {str(e)}")
        return jsonify({'error': str(e)}), 500

# 新增：获取任务文件列表
@app.route('/task_files/<int:task_id>')
def get_task_files(task_id):
    """获取指定任务保存的文件列表"""
    task_dir = os.path.join(DATA_DIR, f'task_{task_id}')
    
    if not os.path.exists(task_dir):
        return jsonify({'error': 'Task directory not found'}), 404
    
    try:
        files = []
        for f in os.listdir(task_dir):
            file_path = os.path.join(task_dir, f)
            if os.path.isfile(file_path):
                files.append({
                    'name': f,
                    'size': os.path.getsize(file_path),
                    'modified': datetime.fromtimestamp(os.path.getmtime(file_path)).isoformat()
                })
        return jsonify({
            'task_id': task_id,
            'directory': task_dir,
            'files': files
        })
    except Exception as e:
        logger.error(f"List files error: {str(e)}")
        return jsonify({'error': str(e)}), 500

# 新增：预览文件内容（前N个字符）
@app.route('/preview/<int:task_id>/<file_type>')
def preview_file(task_id, file_type):
    """预览文件内容"""
    valid_types = {
        'input': 'input.txt',
        'expected': 'expected.txt',
        'output': 'output.txt',
        'log': 'log.txt',
        'summary': 'summary.txt'
    }
    
    if file_type not in valid_types:
        return jsonify({'error': 'Invalid file type'}), 400
    
    task_dir = os.path.join(DATA_DIR, f'task_{task_id}')
    file_path = os.path.join(task_dir, valid_types[file_type])
    
    if not os.path.exists(file_path):
        return jsonify({'error': 'File not found'}), 404
    
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read(10000)  # 最多读取10000字符
            total_size = os.path.getsize(file_path)
            return jsonify({
                'task_id': task_id,
                'file_type': file_type,
                'content': content,
                'truncated': total_size > 10000,
                'total_size': total_size,
                'preview_size': len(content)
            })
    except Exception as e:
        logger.error(f"Preview error: {str(e)}")
        return jsonify({'error': str(e)}), 500

@app.route('/health')
def health():
    """健康检查接口"""
    backend_ok, msg = check_backend()
    data_dir_exists = os.path.exists(DATA_DIR)
    
    logger.info(f"Health check: backend_ok={backend_ok}, data_dir_exists={data_dir_exists}")
    return jsonify({
        'status': 'ok' if backend_ok else 'error',
        'backend': 'found' if backend_ok else 'not found',
        'backend_path': JUDGE_EXE,
        'data_directory': DATA_DIR,
        'data_dir_exists': data_dir_exists,
        'message': msg,
        'timestamp': datetime.now().isoformat()
    })

@app.route('/logs')
def list_logs():
    """列出所有日志文件"""
    logs = []
    log_dirs = ['logs', DATA_DIR]
    
    for log_dir in log_dirs:
        if os.path.exists(log_dir):
            for file in os.listdir(log_dir):
                if file.endswith('.log') or file.endswith('.txt') or file.endswith('.json'):
                    file_path = os.path.join(log_dir, file)
                    logs.append({
                        'name': file,
                        'path': file_path,
                        'directory': log_dir,
                        'size': os.path.getsize(file_path)
                    })
    return jsonify({'logs': logs})

if __name__ == '__main__':
    # 创建必要的目录
    os.makedirs('logs', exist_ok=True)
    os.makedirs(DATA_DIR, exist_ok=True)
    
    host = '127.0.0.1'
    port = 5000
    debug = True
    
    logger.info(f"Starting Judge System Server...")
    logger.info(f"Frontend dir: {FRONTEND_DIR}")
    logger.info(f"Backend dir: {BACKEND_DIR}")
    logger.info(f"Backend exe: {JUDGE_EXE}")
    logger.info(f"Data directory: {DATA_DIR}")
    logger.info(f"Access URL: http://{host}:{port}")
    
    try:
        app.run(debug=debug, host=host, port=port)
    except Exception as e:
        logger.error(f"Server startup failed: {str(e)}")
        logger.error(traceback.format_exc())