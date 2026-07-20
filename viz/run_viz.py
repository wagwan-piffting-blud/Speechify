"""Launch the Speechify Visualizer. Run from the project root."""
import os
import sys

# Ensure project root is on the path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from viz.app import main as app

if __name__ == '__main__':
    app()
