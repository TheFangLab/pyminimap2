import shlex
from ._core import main as _c_main

def main(args):
    """
    Run minimap2 with the given arguments.
    
    Args:
        args (str or list): Command line arguments as a single string (e.g. "-a file1 file2") 
                            or a list of strings (e.g. ["-a", "file1", "file2"]).
    
    Returns:
        tuple: (stdout_str, stderr_str)
    """
    if isinstance(args, str):
        # Use shlex to parse command line string respecting quotes
        args_list = shlex.split(args)
    elif isinstance(args, list):
        args_list = args
    else:
        raise TypeError("Arguments must be a string or a list of strings")
    
    return _c_main(args_list)
