"""Creates a trivial scene (a cube) in Maya standalone and exports it as binary FBX, so we have a
known-good reference file to diff our own writer's structure against.

Usage: mayapy.exe maya_export_reference.py <output-path.fbx>
"""

import sys

import maya.standalone

maya.standalone.initialize(name="python")

import maya.cmds as cmds  # noqa: E402
import maya.mel as mel  # noqa: E402


def main():
    path = sys.argv[1].replace("\\", "/")
    cmds.loadPlugin("fbxmaya", quiet=True)
    cmds.file(new=True, force=True)
    cmds.polyCube(name="RefCube")
    cmds.select("RefCube")
    mel.eval('FBXExportFileVersion -v FBX201400')
    mel.eval('FBXExport -f "%s" -s' % path)
    print("exported to", path)


if __name__ == "__main__":
    main()
