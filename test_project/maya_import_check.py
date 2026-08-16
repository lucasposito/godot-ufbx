"""Imports a given .fbx into Maya standalone (mayapy) and reports what came through, so
FbxSceneWriter's binary output can be checked against the real Autodesk FBX SDK reader, not just
our own ufbx-based one.

Usage: mayapy.exe maya_import_check.py <path-to.fbx>
"""

import sys

import maya.standalone

maya.standalone.initialize(name="python")

import maya.cmds as cmds  # noqa: E402


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: mayapy.exe maya_import_check.py <path-to.fbx>")
        return 1
    path = sys.argv[1].replace("\\", "/")

    cmds.loadPlugin("fbxmaya", quiet=True)
    print("fbxmaya loaded:", cmds.pluginInfo("fbxmaya", query=True, loaded=True))

    cmds.file(new=True, force=True)
    try:
        new_nodes = cmds.file(path, i=True, type="FBX", returnNewNodes=True)
    except Exception as e:  # noqa: BLE001
        print("IMPORT FAILED:", e)
        return 1

    print("new_nodes returned by cmds.file():", new_nodes)
    print("all objects in scene after import:", cmds.ls())

    meshes = cmds.ls(type="mesh") or []
    joints = cmds.ls(type="joint") or []
    skins = cmds.ls(type="skinCluster") or []

    print("meshes:", meshes)
    print("joints:", joints)
    print("skinClusters:", skins)

    for m in meshes:
        vtx_count = cmds.polyEvaluate(m, vertex=True)
        face_count = cmds.polyEvaluate(m, face=True)
        print("  mesh", m, "vertices=", vtx_count, "faces=", face_count)

    for s in skins:
        infs = cmds.skinCluster(s, query=True, influence=True) or []
        print("  skinCluster", s, "influences=", len(infs), infs)

    return 0


if __name__ == "__main__":
    sys.exit(main())
