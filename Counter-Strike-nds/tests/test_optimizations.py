"""Host regressions for hardware-independent game logic.

Run: python3 -m unittest discover -s Counter-Strike-nds/tests -v
The C harnesses compile actual function bodies with minimal platform types.
They do not replace a devkitARM build or hardware performance measurements.
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "source"


def source(path):
    return (SOURCE / path).read_text()


def function(path, name):
    text = source(path)
    match = re.search(r"^(?:static )?(?:inline )?[\w *]+\b" + name + r"\([^;]*?\)\s*\{", text, re.M)
    assert match, name
    start = text.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


def run_c(body):
    with tempfile.TemporaryDirectory(prefix="csds-test-") as directory:
        path = Path(directory)
        (path / "test.c").write_text(
            "#include <assert.h>\n#include <stdbool.h>\n#include <stdlib.h>\n"
            "#include <string.h>\n#include <math.h>\n#include <stdio.h>\n" + body
        )
        subprocess.run([os.environ.get("CC", "clang"), "-std=c99", "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(path / "test.c"), "-lm", "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True, timeout=30)


class OptimizationTests(unittest.TestCase):
    def test_matrix_payload_equivalence(self):
        text = source("ai/data/ai_data.c")
        maps = re.split(r"(?:if|else if) \(mapToLoad == \w+\)", text[text.index("void createLengthMatrices"):])[1:]
        expanded = bytearray()
        count = 0
        packed_bytes = 0
        for block in maps:
            size = int(re.search(r"MatricesSize = (\d+)", block)[1])
            for length, body in re.findall(r"static const unsigned char matrix\d+\[(\d+)\] = \{(.*?)\};", block, re.S):
                data = bytes(int(v, 16) for v in re.findall(r"0x[\da-f]+", body))
                self.assertEqual(len(data), int(length))
                self.assertEqual(len(data), (size * size + 7) // 8)
                expanded.extend((data[i >> 3] >> (i & 7)) & 1 for i in range(size * size))
                packed_bytes += len(data)
                count += 1
        self.assertEqual(count, 89)
        self.assertEqual(len(expanded), 171845)
        self.assertEqual(hashlib.sha256(expanded).hexdigest(), "518d3412a8045a706fe5b7d0a93190ab5293c7f08e1331ca51b0c2862c3f7878")
        self.assertLess(packed_bytes, 21600)

    def test_paths_and_nearest_waypoint(self):
        preamble = r"""
        enum { DUST2, TUTORIAL, DUST2_2x2, AIM_MAP, B2000, FYSNOW, MIRAGE };
        typedef struct { float x,y,z; int edgeCount; int *edge; } Waypoint;
        typedef struct { const unsigned char *matrixOneLength; } PathLength;
        typedef struct { int Path[maxPath], PathCount, CurrentPath; } Player;
        Waypoint Waypoints[maxPoint];
        Player AllPlayers[1];
        PathLength *AllMatricesLength;
        int MatricesSize, MatriceCount, waypointsSize, SiteAPoint, SiteBPoint;
        """
        preamble = "\n".join(re.findall(r"^#define (?:maxPoint|maxPath) .*$", source("main.h"), re.M)) + "\n" + preamble
        funcs = "\n".join([
            function("main.h", "squareFloat"),
            function("ai/ai.c", "CreateWaypoint"), function("ai/ai.c", "freeWaypoint"),
            function("ai/data/ai_data.c", "CreateWaypoints"),
            function("ai/data/ai_data.c", "createLengthMatrices"),
            function("ai/ai.c", "pathExists"), function("ai/ai.c", "CheckPathWaypoint"),
            function("ai/ai.c", "getNearestWaypoint"),
        ])
        run_c(preamble + funcs + r"""
        int main(void) {
            int longest = 0;
            for (int pass=0; pass<3; pass++) for (int map=0; map<7; map++) {
                CreateWaypoints(map); createLengthMatrices(map);
                assert(waypointsSize == MatricesSize);
                for (int a=0; a<waypointsSize; a++) for (int b=0; b<waypointsSize; b++) {
                    CheckPathWaypoint(0,a,b);
                    Player *p=&AllPlayers[0];
                    assert(p->PathCount > 0 && p->PathCount <= maxPath);
                    assert(p->Path[0]==a && p->Path[p->PathCount-1]==b);
                    if (p->PathCount > longest) longest = p->PathCount;
                    for (int i=1; i<p->PathCount; i++) {
                        Waypoint *w=&Waypoints[p->Path[i-1]];
                        bool connected=false;
                        for (int e=0;e<w->edgeCount;e++) connected |= w->edge[e]==p->Path[i];
                        assert(connected);
                    }
                    int expected=1;
                    if(a!=b) { while(!pathExists(expected-1,a,b)) expected++; expected++; }
                    assert(p->PathCount==expected);
                }
                for(int i=0;i<1000;i++) {
                    float x=(rand()%20000-10000)/100.0f, y=(rand()%2000)/100.0f, z=(rand()%20000-10000)/100.0f;
                    int best=-1, distance=99999;
                    for(int w=0;w<waypointsSize;w++) {
                        int d=sqrtf(powf(Waypoints[w].x-x,2)+powf(Waypoints[w].y-y,2)+powf(Waypoints[w].z-z,2));
                        if(d<distance) {best=w;distance=d;}
                    }
                    assert(getNearestWaypoint(x,y,z)==best);
                }
                CheckPathWaypoint(0,-1,0); assert(AllPlayers[0].PathCount==0);
                CheckPathWaypoint(0,0,waypointsSize); assert(AllPlayers[0].PathCount==0);
            }
            assert(longest>15);
            freeWaypoint();
            puts("All map routes and nearest-waypoint comparisons passed");
        }
        """)

    def test_wall_masks(self):
        run_c(r"""
        typedef struct { int ZoneCollision; } Wall;
        typedef struct { int ZoneCount, visibleMapPart[4]; } Zone;
        typedef struct { Wall *AllWallsCollisions; Zone *AllZones; int CollisionsCount, zonesCount;
            unsigned char *raycastWallMasks; } Map;
        Map allMaps[1];
        """ + function("collisions/raycast.c", "wallVisibleFromZone") +
              function("collisions/raycast.c", "BuildRaycastWallMasks") + r"""
        int main(void) {
            Wall walls[213]; Zone zones[3]={{2,{0,2}},{1,{1}},{0,{0}}};
            allMaps[0]=(Map){walls,zones,213,3,NULL};
            for(int i=0;i<213;i++) walls[i].ZoneCollision=i%5-1;
            for(int repeat=0;repeat<10;repeat++) {
                BuildRaycastWallMasks(0);
                for(int z=0;z<3;z++) for(int w=0;w<213;w++) {
                    bool expected=walls[w].ZoneCollision==-1;
                    for(int i=0;i<zones[z].ZoneCount;i++) expected |= zones[z].visibleMapPart[i]==walls[w].ZoneCollision;
                    bool actual=(allMaps[0].raycastWallMasks[z*27+(w>>3)]>>(w&7))&1;
                    assert(actual==expected);
                }
            }
            free(allMaps[0].raycastWallMasks);
        }
        """)

    def test_player_collision_cache(self):
        run_c(r"""
        #define MaxPlayer 2
        typedef struct {float x,y,z;} Vector3;
        typedef struct {int x,y,z;} Vector3Int;
        typedef struct {int BoxXRangeA,BoxXRangeB,BoxYRangeA,BoxYRangeB,BoxZRangeA,BoxZRangeB;} CollisionBox;
        typedef struct {Vector3Int *PlayerModel; Vector3 position; float xSize,ySize,zSize; CollisionBox PlayerCollisionBox;} Player;
        Player AllPlayers[MaxPlayer];
        """ + function("player/player.c", "CalculatePlayerPosition") +
              function("collisions/collisions.c", "CalculatePlayerColBox") + r"""
        int main(void) {
            Vector3Int model={12345,8765,-6543};
            AllPlayers[0]=(Player){&model,{0},0.35f,0.9f,0.35f,{0}};
            CalculatePlayerColBox(0);
            CollisionBox original=AllPlayers[0].PlayerCollisionBox;
            memset(&AllPlayers[0].PlayerCollisionBox,0,sizeof(CollisionBox));
            CalculatePlayerColBox(0);
            assert(memcmp(&original,&AllPlayers[0].PlayerCollisionBox,sizeof(original))==0);
            model.x+=4096;
            CalculatePlayerColBox(0);
            assert(AllPlayers[0].PlayerCollisionBox.BoxXRangeA==original.BoxXRangeA+4096);
            AllPlayers[0].xSize=1.0f;
            CalculatePlayerColBox(0);
            assert(AllPlayers[0].PlayerCollisionBox.BoxXRangeA==(int)((AllPlayers[0].position.x+1.0f)*4096.0));
            // Reused player slots must restore boxes even at identical coordinates.
            AllPlayers[1]=AllPlayers[0];
            CalculatePlayerColBox(1);
            assert(memcmp(&AllPlayers[0].PlayerCollisionBox,&AllPlayers[1].PlayerCollisionBox,sizeof(original))==0);
        }
        """)

    def test_map_lifecycle(self):
        loader = function("map/map.c", "loadMapModels")
        assets = sorted(set(re.findall(r"\b\w+_bin\b", loader)))
        run_c(r"""
        typedef unsigned int u32;
        typedef struct {float x,y,z;} Vector3;
        typedef struct {int x,y,z;} Vector3Int;
        typedef struct {int rx,ry,loaded;} NE_Model;
        typedef struct {NE_Model *Model;} MapModel;
        typedef struct {void *WallPhysics; NE_Model *WallModel;} Wall;
        typedef struct {int *nearWaypoints;} Site;
        typedef struct {
            MapModel *models; int modelCount,occlusionZoneCount,CollisionsCount,BombsTriggersCollisionsCount;
            Wall *AllWallsCollisions; Site *AllBombsTriggersCollisions;
            void *AllStairs,*AllZones,*AllShadowCollisionBox,*AllOcclusionZone,*raycastWallMasks;
        } Map;
        enum {DUST2,TUTORIAL,DUST2_2x2,AIM_MAP,B2000,FYSNOW,MIRAGE,NE_Static};
        #define GrenadeCount 10
        Map allMaps[7]; int CurrentTexture=1, LastStairs; void *GroundMaterial;
        int liveModels;
        NE_Model *NE_ModelCreate(int type) {liveModels++; return calloc(1,sizeof(NE_Model));}
        void NE_ModelDelete(NE_Model *m) {assert(m); liveModels--;free(m);}
        void NE_PhysicsDelete(void *p) {free(p);}
        void DeleteGrenade(int i) {(void)i;}
        void TextureToLoad(int i) {CurrentTexture=i;}
        #define NE_ModelSetMaterial(...) ((void)0)
        #define NE_ModelScaleI(...) ((void)0)
        #define NE_ModelSetCoord(...) ((void)0)
        void NE_ModelLoadStaticMesh(NE_Model *m,u32 *mesh) {m->loaded=1;}
        """ + "\n".join(f"u32 {asset}[1];" for asset in assets) + loader +
              function("map/map.c", "UnLoadMap") + r"""
        int main(void) {
            int counts[7]={7,1,4,3,2,2,6};
            for(int repeat=0;repeat<10;repeat++) for(int m=0;m<7;m++) {
                Map *map=&allMaps[m];
                loadMapModels(m);
                assert(map->modelCount==counts[m]);
                for(int i=0;i<map->modelCount;i++) assert(map->models[i].Model->loaded);
                // Visibility regions need not equal the number of model objects.
                map->occlusionZoneCount=8;
                map->AllOcclusionZone=malloc(512);
                map->raycastWallMasks=malloc(27);
                map->AllWallsCollisions=calloc(2,sizeof(Wall)); map->CollisionsCount=2;
                for(int i=0;i<2;i++) {
                    map->AllWallsCollisions[i].WallModel=NE_ModelCreate(NE_Static);
                    map->AllWallsCollisions[i].WallPhysics=malloc(1);
                }
                map->BombsTriggersCollisionsCount=1;
                map->AllBombsTriggersCollisions=malloc(sizeof(Site));
                map->AllBombsTriggersCollisions[0].nearWaypoints=malloc(sizeof(int));
                map->AllStairs=malloc(1);map->AllZones=malloc(1);map->AllShadowCollisionBox=malloc(1);
                UnLoadMap(m); UnLoadMap(m);
                assert(liveModels==0 && map->models==NULL && map->AllOcclusionZone==NULL);
                assert(map->raycastWallMasks==NULL);
            }
        }
        """)

    def test_packet_framing(self):
        # Keep the actual framing/tokenization/compaction; replace only dispatch.
        text = function("network/network.c", "treatData")
        start = text.index("        // Check packet info")
        end = text.rindex("    }\n    if (!start)")
        text = text[:start] + "        received++;\n" + text[end:]
        run_c("char Values[1024]; int received;\n" + text + r"""
        int main(void) {
            const char *packets="{POS;1;2}{PING}{NAME;abc}";
            for(size_t split=0;split<=strlen(packets);split++) {
                received=0;
                memcpy(Values,packets,split); Values[split]=0; treatData();
                strcat(Values,packets+split); treatData();
                assert(received==3 && Values[0]==0);
            }
            strcpy(Values,"noise{PING}{POS;12"); received=0; treatData();
            assert(received==1 && strcmp(Values,"{POS;12")==0);
            strcpy(Values,"garbage"); treatData(); assert(Values[0]==0);
            strcpy(Values,"{}"); treatData(); assert(Values[0]==0);
            Values[0]='{'; memset(Values+1,'x',300); strcpy(Values+301,"}{PING}");
            received=0; treatData(); assert(received==1 && Values[0]==0);
            strcpy(Values,"{A;1;2;3;4;5;6;7;8;9;10}{PING}");
            received=0; treatData(); assert(received==1);
            Values[0]='{'; memset(Values+1,'x',64); strcpy(Values+65,"}{PING}");
            received=0; treatData(); assert(received==1);
        }
        """)

    def test_trig_precision(self):
        run_c("#define M_TWOPI 6.28318530717958647692\n" +
              function("player/movements.c", "UpdateLookRotationAI") + r"""
        int main(void) {
            for(int a=-1024;a<=1024;a++) for(int t=0;t<=512;t+=4) {
                float x,y,z;
                UpdateLookRotationAI(t,a/2.0f,&x,&y,&z);
                float s=(a/2.0f)/512.0*M_TWOPI, p=(384-t)/512.0*M_TWOPI;
                assert(fabs(x-sin(s)*cos(p))<0.000001);
                assert(fabs(y+sin(p))<0.000001);
                assert(fabs(z-cos(s)*cos(p))<0.000001);
                assert(fabs(x*x+y*y+z*z-1)<0.000001);
            }
        }
        """)


if __name__ == "__main__":
    unittest.main()
