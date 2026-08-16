"""Creates a trivial SKINNED scene (a plane bound to a 2-joint chain) in Maya standalone and
exports it as binary FBX, to get an authoritative Deformer/Pose Definitions sample (the cube-only
reference in maya_export_reference.py has no skin, so no Deformer/Pose templates to compare
against).

Usage: mayapy.exe maya_export_reference2.py <output-path.fbx>
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

    j0 = cmds.joint(name="Bone0", position=(0, 0, 0))
    j1 = cmds.joint(name="Bone1", position=(0, 1, 0))
    cmds.select(clear=True)

    plane = cmds.polyPlane(name="SkinnedPlane", width=1, height=2, subdivisionsX=1, subdivisionsY=1)[0]
    cmds.select([j0, plane])
    cmds.skinCluster(toSelectedBones=True)

    cmds.select([j0, plane])
    mel.eval('FBXExportFileVersion -v FBX201400')
    mel.eval('FBXExport -f "%s" -s' % path)
    print("exported to", path)


if __name__ == "__main__":
    main()
