import argparse
import os
import subprocess
import sys
"""
1. 格式化单个指定的文件夹
python format_code.py ./src


2. 同时格式化多个指定的路径（混合文件和文件夹）
python format_code.py ./src /Users/project/include ./main.cpp

3. 指定特定的文件后缀
如果你只想对 ./src 目录下的 .hpp 和 .cc 文件进行对齐，可以加上 --ext 参数：
python format_code.py ./src --ext hpp cc


"""



def batch_format(target_paths, extensions):
    # 转换为绝对路径并去重
    valid_paths = [os.path.abspath(p) for p in target_paths if os.path.exists(p)]

    if not valid_paths:
        print("错误: 未找到任何有效的指定路径！")
        sys.exit(1)

    print(f"正在扫描路径: {valid_paths}")
    formatted_count = 0

    for base_path in valid_paths:
        # 如果传入的是单个文件，直接处理
        if os.path.isfile(base_path):
            if base_path.endswith(extensions):
                if run_clang_format(base_path):
                    formatted_count += 1
            continue

        # 如果是文件夹，递归遍历
        for root, _, files in os.walk(base_path):
            for name in files:
                if name.endswith(extensions):
                    file_path = os.path.join(root, name)
                    if run_clang_format(file_path):
                        formatted_count += 1

    print(f"\n处理完成！共成功格式化 {formatted_count} 个文件。")


def run_clang_format(file_path):
    try:
        # 使用 subprocess 代替 os.system，能更好地处理带有空格或特殊字符的路径
        print(f"Formatting: {file_path}")
        subprocess.run(["clang-format", "-i", file_path, "-style=File"], check=True)
        return True
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"格式化失败 [{file_path}]: {e}")
        return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Clang-format 跨平台批量格式化工具")

    # 添加位置参数，nargs='+' 表示支持接收 1 个或多个路径
    parser.add_argument(
        "paths",
        nargs="+",
        help="指定要格式化的文件或文件夹路径（支持多个，用空格隔开）",
    )

    # 允许自定义文件后缀，默认支持常见 C/C++ 后缀
    parser.add_argument(
        "--ext",
        nargs="+",
        default=[".h", ".cpp", ".c", ".cc", ".hpp"],
        help="指定处理的文件后缀，默认: .h .cpp .c .cc .hpp",
    )

    args = parser.parse_args()

    # 规范化后缀格式（确保带 .）
    exts = tuple(e if e.startswith(".") else f".{e}" for e in args.ext)

    batch_format(args.paths, exts)
