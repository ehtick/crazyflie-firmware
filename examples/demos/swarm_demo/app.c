#include <float.h>
#include <math.h>

#include "FreeRTOS.h"
#include "timers.h"
#include "deck_digital.h"
#include "deck_constants.h"
#include "sensors.h"
#include "estimator_kalman.h"
#include "crtp_commander_high_level.h"
#include "commander.h"
#include "pm.h"
#include "stabilizer.h"
#include "ledseq.h"
#include "log.h"
#include "param.h"
#include "supervisor.h"
#include "controller.h"
#include "ledseq.h"
#include "pptraj.h"
#include "lighthouse_position_est.h"
#include "lighthouse_core.h"

#define DEBUG_MODULE "APP"
#include "debug.h"


static xTimerHandle timer;
static bool isInit = false;

static void appTimer(xTimerHandle timer);

#define LED_LOCK         LED_GREEN_R
#define LOCK_LENGTH 50
#define LOCK_THRESHOLD 0.001f
#define MAX_PAD_ERR 0.005
#define TAKE_OFF_HEIGHT 0.2f
#define LANDING_HEIGHT 0.12f
#define SEQUENCE_SPEED 1.0f
#define DURATION_TO_INITIAL_POSITION 2.0

static uint32_t lockWriteIndex;
static float lockData[LOCK_LENGTH][3];
static void resetLockData();
static bool hasLock();

static bool takeOffWhenReady = false;
static float goToInitialPositionWhenReady = -1.0f;
static bool terminateTrajectoryAndLand = false;

static float padX = 0.0;
static float padY = 0.0;
static float padZ = 0.0;

static uint32_t landingTimeCheckCharge = 0;

static float stabilizeEndTime;

#define NO_PROGRESS -2000.0f
static float currentProgressInTrajectory = NO_PROGRESS;
static uint32_t trajectoryStartTime = 0;
static uint32_t timeWhenToGoToInitialPosition = 0;
static float trajectoryDurationMs = 0.0f;

static float trajecory_center_offset_x = 0.0f;
static float trajecory_center_offset_y = 0.0f;
static float trajecory_center_offset_z = 0.0f;

static uint32_t now = 0;
static uint32_t flightTime = 0;

// The nr of trajectories to fly
static uint8_t trajectoryCount = 255;
static uint8_t remainingTrajectories = 0;

// Log and param ids
static logVarId_t logIdStateEstimateX;
static logVarId_t logIdStateEstimateY;
static logVarId_t logIdStateEstimateZ;
static logVarId_t logIdKalmanVarPX;
static logVarId_t logIdKalmanVarPY;
static logVarId_t logIdKalmanVarPZ;
static logVarId_t logIdPmState;
static logVarId_t logIdlighthouseEstBs0Rt;
static logVarId_t logIdlighthouseEstBs1Rt;

static paramVarId_t paramIdStabilizerController;
static paramVarId_t paramIdCommanderEnHighLevel;
static paramVarId_t paramIdLighthouseMethod;

//#define USE_MELLINGER

#define TRAJ_Y_OFFSET 0.35

enum State {
  // Initialization
  STATE_IDLE = 0,
  STATE_WAIT_FOR_POSITION_LOCK,

  STATE_WAIT_FOR_TAKE_OFF, // Charging
  STATE_TAKING_OFF,
  STATE_HOVERING,
  STATE_WAITING_TO_GO_TO_INITIAL_POSITION,
  STATE_GOING_TO_INITIAL_POSITION,
  STATE_RUNNING_TRAJECTORY,
  STATE_GOING_TO_PAD,
  STATE_WAITING_AT_PAD,
  STATE_LANDING,
  STATE_CHECK_CHARGING,
  STATE_REPOSITION_ON_PAD,
  STATE_CRASHED,
};

static enum State state = STATE_IDLE;

ledseqStep_t seq_lock_def[] = {
  { true, LEDSEQ_WAITMS(1000)},
  {    0, LEDSEQ_LOOP},
};

ledseqContext_t seq_lock = {
  .sequence = seq_lock_def,
  .led = LED_LOCK,
};

const uint8_t trajectoryId = 1;

// duration, x0-x7, y0-y7, z0-z7, yaw0-yaw7
static struct poly4d sequence[] = {
  {.duration = 0.55, .p = {{3.850187562676742e-11,-6.126680001583386e-07,0.034011535082562466,-0.0003930116221214229,-0.13617012073728407,-0.013475566231476182,0.13052917793131502,-0.04578980321072286}, {5.13033102278894e-11,-6.894694832263486e-07,2.7736466486255528e-05,0.09665022145136354,0.003058298046984803,-0.14597702960520773,0.025898651835890598,0.031816204956814045}, {0.6948075950058442,0.2683789944542717,0.023245728473115564,-0.010134659315685406,-0.00043890670824819824,0.00011480619184472054,3.331435279368211e-06,-6.413660347948354e-07}, {0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,}}},
  {.duration = 0.55, .p = {{-6.914788682055622e-11,-0.029193709157554027,-0.10559495462082236,-0.05347086572904071,0.1429864490023195,0.13154711964776383,-0.10576667456482508,-0.004894549656606144}, {0.010222252124903127,0.03695902744452226,-0.0088512292032624,-0.1522994840241329,-0.10473818933895811,0.101347754562758,0.10503082012800732,-0.06287119477122378}, {0.8477274186362265,0.2845134091691316,0.005921828500929774,-0.01074393477303553,-0.00011181155704426794,0.00012171542649083367,8.448121214720302e-07,-6.586027744088609e-0}, {0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,}}},
  {.duration = 0.55, .p = {{-0.04019237884462183,-0.07140003156906653,0.1344931065364669,0.2937887095214804,0.009789196771361027,-0.21053548856185011,-0.05387360520003008,0.07164061868443176}, {-7.889581297779964e-11,-0.11478801738672623,-0.20396423865607044,0.07270282032653184,0.2794859692648988,0.07498870791184684,-0.17675426223153057,0.027076140218982695}, {1.0042095323020248,0.28125870522972984,-0.011805634296686286,-0.010621028836870822,0.00022290339530923512,0.00012032988892924413,-1.6993066948952098e-06,-6.309899868949724e-07}, {0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,}}},
  {.duration = 0.55, .p = {{7.988284484732011e-11,0.250949124563498,0.2884318844281135,-0.2732723085219975,-0.3971380460135978,0.019843843315547182,0.23402372804388663,-0.06191711498753417}, {-0.08786796558968513,-0.1009753021910108,0.33435181061014774,0.4152184312151822,-0.14220624001585802,-0.30664667217939623,0.019456187255464174,0.07150045317180133}, {1.153589938555835,0.25883668533089255,-0.028728562626473653,-0.009774317339295326,0.0005424278565983879,0.00011074405151188298,-4.127597844930454e-06,-5.60388917887036e-07}, {-8.314418855166948e-17,2.855993321445286,-4.4522405463652314e-13,3.6027429443266216e-12,-1.4127312536830109e-11,2.8100825191264194e-11,-2.6527583249725554e-11,8.815269909223687e-12,}}},
  {.duration = 0.55, .p = {{0.1499999999110494,0.1236693334929018,-0.5948073010234445,-0.5083134142475911,0.34088988320791197,0.38313148703735755,-0.10996125300699039,-0.06246025031151826}, {7.204165642107989e-11,0.42839787625064485,0.3532415614668606,-0.5345691196919508,-0.48792488465828615,0.14648785242312232,0.27367225249021054,-0.09725401975942494}, {1.285688609574253,0.21877537307462952,-0.043693686890084134,-0.008261502261561984,0.0008249867228197418,9.361128232250471e-05,-6.274729564937099e-06,-4.5152734395454723e-07}, {1.570796326794896,2.855993321445219,1.1415585581287713e-12,-1.1944049757036972e-11,6.431141607525981e-11,-1.8343792153223745e-10,2.6289169140858755e-10,-1.4907548089075316e-10,}}},
  {.duration = 0.55, .p = {{-5.590663147591574e-11,-0.6350414332686105,-0.39397659738539126,0.8387863079720367,0.5456595121538056,-0.29631273940020514,-0.29299785427839664,0.130678702950142}, {0.2223542863477553,0.13793556474518312,-0.8981099665280236,-0.5667293893693737,0.5727217708530097,0.43477761941329673,-0.21147382139585272,-0.04513608499171106}, {1.3915032392307496,0.1638048806867086,-0.05568115859886775,-0.006185679463293696,0.001051324141053692,7.009895638961352e-05,-7.994094322078248e-06,-3.1198566063354317e-07}, {-3.1415926535897913,2.855993321445259,2.5087977390525737e-13,-1.1471253057481462e-12,-3.6097331487140355e-12,3.293016202670872e-11,-7.167566690499768e-11,5.132340432324304e-11,}}},
  {.duration = 0.55, .p = {{-0.2999999998502529,-0.14280177586409412,1.2235902317016245,0.5864854044065786,-0.821902942890023,-0.4580654708546404,0.3170759788277885,0.020708570338654426}, {-3.2577411606298086e-11,-0.8567973787012538,-0.40786096680320616,1.16519197471624,0.5664074090974773,-0.4591081859649081,-0.29068352530931285,0.1599133274637631}, {1.4638227353811781,0.09767135638036385,-0.06387405136662831,-0.0036883128327376896,0.0012060155635279994,4.180948131089076e-05,-9.168649106299251e-06,-1.5119295149245228e-07}, {-1.570796326794896,2.855993321445285,-4.225442457827085e-13,4.4049449702295166e-12,-2.3950348360899724e-11,6.851557251898727e-11,-9.803060051919491e-11,5.52677029888256e-11,}}},
  {.duration = 0.55, .p = {{3.643713234144152e-12,1.0785534113363267,0.3939484728911548,-1.4915421131692534,-0.5487546404587444,0.6237799510123201,0.266886983994221,-0.18296560239343668}, {-0.3776457133589556,-0.1379363426036038,1.5490671543520358,0.5662351196076164,-1.0714521143729914,-0.4514080123736052,0.41957111221051374,-0.00915759855778746}, {1.497718643876504,0.02488169054632043,-0.06771403309317417,-0.0009395937484483393,0.0012785188596592392,1.0671200625251397e-05,-9.718988081428503e-06,2.0238909278164195e-08}, {-8.314418855166948e-17,2.855993321445286,-4.4522405463652314e-13,3.6027429443266216e-12,-1.4127312536830109e-11,2.8100825191264194e-11,-2.6527583249725554e-11,8.815269909223687e-12,}}},
  {.duration = 0.55, .p = {{0.44999999981385364,0.12367083619999372,-1.8523600200796242,-0.5073585583818294,1.3043629214703734,0.41525893933383995,-0.5119743484215598,0.04242709212774574}, {-2.89226389768401e-11,1.285197224018999,0.35318722911504113,-1.7955965006790715,-0.4939042134616532,0.7791059265493233,0.223229924284725,-0.19826455269120852}, {1.4908810145684093,-0.049603621379605944,-0.06693941536376884,0.001873157056002599,0.0012638934376494498,-2.1194960501981645e-05,-9.606060589372755e-06,1.8976712320425972e-07}, {1.570796326794896,2.855993321445219,1.1415585581287713e-12,-1.1944049757036972e-11,6.431141607525981e-11,-1.8343792153223745e-10,2.6289169140858755e-10,-1.4907548089075316e-10,}}},
  {.duration = 0.55, .p = {{6.290210506218638e-11,-1.4626463824098959,-0.28835504688017033,2.056634333247623,0.40559409384743944,-0.914500903393737,-0.16268750359645887,0.20476758072452098}, {0.5121320341641584,0.10097742733972069,-2.11279992129731,-0.41386806111463986,1.504762877791794,0.35208175069725167,-0.5879885586908895,0.07683264882809585}, {1.443775820594523,-0.12070852848241553,-0.06160298710010411,0.004558255313738797,0.0011631356744089595,-5.161639604707585e-05,-8.839022757646207e-06,3.4668809927088064e-07}, {-3.1415926535897913,2.855993321445259,2.5087977390525737e-13,-1.1471253057481462e-12,-3.6097331487140355e-12,3.293016202670872e-11,-7.167566690499768e-11,5.132340432324304e-11,}}},
  {.duration = 0.55, .p = {{-0.5598076209469373,-0.07140263433406184,2.3126383091368354,0.29213485066461087,-1.6589950573170387,-0.26618186797986115,0.6424335009665487,-0.1100295872652435}, {9.597957855088104e-11,-1.5988080196133752,-0.20387013226277426,2.256866313938279,0.28984247051151446,-1.0207379381606654,-0.08938558706550837,0.20203151543026565}, {1.3596132030791528,-0.18358734884936753,-0.05206841706985553,0.006932716003333422,0.0009831121712365765,-7.852040976178658e-05,-7.4693513883816284e-06,4.798093435152236e-07}, {-1.570796326794896,2.855993321445285,-4.225442457827085e-13,4.4049449702295166e-12,-2.3950348360899724e-11,6.851557251898727e-11,-9.803060051919491e-11,5.52677029888256e-11,}}},
  {.duration = 0.55, .p = {{-1.2590019835852742e-10,1.6844029450716946,0.10548999257784991,-2.3826469641845596,-0.1545376251943579,1.09057715220263,0.00831957959381703,-0.19024281538328217}, {-0.5897777477105767,-0.0369619304516384,2.4382565277141275,0.1504548441642516,-1.7565487919657432,-0.16341322602586436,0.6715988421120441,-0.13975559077778596}, {1.244128705320668,-0.2339549947846461,-0.038985470464346035,0.008834723564599386,0.0007360911420512199,-0.00010007323169172001,-5.590868353790516e-06,5.803448593771976e-07}, {-8.314418855166948e-17,2.855993321445286,-4.4522405463652314e-13,3.6027429443266216e-12,-1.4127312536830109e-11,2.8100825191264194e-11,-2.6527583249725554e-11,8.815269909223687e-12,}}},
  {.duration = 0.55, .p = {{0.5999999998441106,2.392746148312848e-06,-2.4810939030286048,0.001516700094742569,1.7907759558714795,0.05077933811777355,-0.6734970125477634,0.1639848814705509}, {-1.506250987376076e-10,1.7135980060672011,-8.092823644568708e-05,-2.4254045405309936,-0.00890035951518397,1.1192591185216,-0.07498600440013853,-0.17020486100199314}, {1.105192404994155,-0.2683789944542932,-0.023245728472413945,0.01013465930675449,0.00043890675937640944,-0.00011480633720814209,-3.331233868784164e-06,6.412574240165199e-07}, {1.570796326794896,2.855993321445219,1.1415585581287713e-12,-1.1944049757036972e-11,6.431141607525981e-11,-1.8343792153223745e-10,2.6289169140858755e-10,-1.4907548089075316e-10,}}},
  {.duration = 0.55, .p = {{1.684697291033214e-10,-1.6844036074401618,0.10564814639079924,2.382225184807529,-0.1371443875231521,-1.1048292086075555,0.1548540272068101,0.14328320564974964}, {0.5897777477577096,-0.036957247366396044,-2.4382311387421707,0.1534231724906406,1.7593440245030696,-0.06404398275378732,-0.6479986546427713,0.18106627297730243}, {0.9522725813637728,-0.2845134091691431,-0.005921828500469207,0.01074393476605049,0.00011181160327455771,-0.00012171557551975708,-8.445807090820611e-07,6.584637616417346e-07}, {-3.1415926535897913,2.855993321445259,2.5087977390525737e-13,-1.1471253057481462e-12,-3.6097331487140355e-12,3.293016202670872e-11,-7.167566690499768e-11,5.132340432324304e-11,}}},
  {.duration = 0.55, .p = {{-0.5598076210379905,0.07139825149092185,2.312589261409516,-0.2949123979936194,-1.664395031908084,0.17323171668372522,0.5968414398024987,-0.18983569693480895}, {1.7821782545551847e-10,-1.5988092992110026,0.20401743042640863,2.25605149874844,-0.2736439077696821,-1.0482707969090843,0.22584161491618038,0.11131251575573874}, {0.7957904676979738,-0.28125870522974444,0.011805634297233001,0.010621028829982298,-0.00022290335524226357,-0.00012033000419331912,1.699466435538814e-06,6.309046379744497e-07}, {-1.570796326794896,2.855993321445285,-4.225442457827085e-13,4.4049449702295166e-12,-2.3950348360899724e-11,6.851557251898727e-11,-9.803060051919491e-11,5.52677029888256e-11,}}},
  {.duration = 0.55, .p = {{-1.7920484419930034e-10,1.462648192034213,-0.2884850761977632,-2.055482010561792,0.39129598457085574,0.9534382455215172,-0.2831110804852299,-0.07647154113270976}, {-0.5121320342929275,0.10097352211288317,2.11273055733514,-0.41634211967852053,-1.5123995951714493,0.2693429004489931,0.5235116471329888,-0.1896955312999179}, {0.6464100614441641,-0.2588366853309645,0.028728562628598252,0.009774317314729044,-0.0005424277188845949,-0.00011074445057684398,4.128172898051939e-06,5.600631449541777e-07}, {-8.314418855166948e-17,2.855993321445286,-4.4522405463652314e-13,3.6027429443266216e-12,-1.4127312536830109e-11,2.8100825191264194e-11,-2.6527583249725554e-11,8.815269909223687e-12,}}},
  {.duration = 0.55, .p = {{0.44999999997156337,-0.12366755341478729,-1.8522750669218213,0.5094371027127081,1.3137159519637331,-0.34582771525204625,-0.43300658146993454,0.180655328493912}, {-1.713634072390606e-10,1.2851994403470866,-0.3532947532373552,-1.7941851993804268,0.48208282314515644,0.8267942366336414,-0.3227596052703462,-0.041134636155708774}, {0.514311390425746,-0.21877537307467282,0.0436936868909435,0.008261502253779647,-0.0008249866851128808,-9.36113819283429e-05,6.274863968829055e-06,4.51455051613041e-07}, {1.570796326794896,2.855993321445219,1.1415585581287713e-12,-1.1944049757036972e-11,6.431141607525981e-11,-1.8343792153223745e-10,2.6289169140858755e-10,-1.4907548089075316e-10,}}},
  {.duration = 0.55, .p = {{1.552285277508215e-10,-1.0785558833291304,0.3940297891560065,1.4899680111000886,-0.5398174506435933,-0.6769693496382162,0.34208520701934,0.0077099529941195115}, {0.3776457135348575,-0.13793378466699563,-1.5489724014191015,0.5678530778537859,1.0818840642180365,-0.3974738473501345,-0.33149401346895935,0.16333116338935125}, {0.4084967607692496,-0.16380488068674098,0.05568115859943449,0.00618567945863584,-0.0010513241211192762,-7.009900241094177e-05,7.994148431007077e-06,3.119604503872529e-07}, {-3.1415926535897913,2.855993321445259,2.5087977390525737e-13,-1.1471253057481462e-12,-3.6097331487140355e-12,3.293016202670872e-11,-7.167566690499768e-11,5.132340432324304e-11,}}},
  {.duration = 0.55, .p = {{-0.30000000003235977,0.14279999578597702,1.2234921362438944,-0.587609092875647,-0.8327028922566038,0.4207616989920525,0.2258918557649712,-0.13890364858850515}, {1.3189926344258986e-10,-0.8567999378964555,0.407914158573091,1.1635623443630034,-0.5605653476226719,-0.5141739029796313,0.3397708779238144,-0.021524671451362908}, {0.3361772646188215,-0.09767135638040796,0.06387405136797168,0.0036883128165148063,-0.001206015469879388,-4.180975783766554e-05,9.169052818128353e-06,1.5096177044506199e-07}, {-1.570796326794896,2.855993321445285,-4.225442457827085e-13,4.4049449702295166e-12,-2.3950348360899724e-11,6.851557251898727e-11,-9.803060051919491e-11,5.52677029888256e-11,}}},
  {.duration = 0.55, .p = {{-1.029656785148531e-10,0.6350439052614014,-0.39400166466153436,-0.8372122059040767,0.5429125789487638,0.3495021380409436,-0.3159743367761405,0.04457694648242842}, {-0.2223542865236572,0.13793456252546576,0.8980152135936907,-0.5673588080774555,-0.5831537207722597,0.4141042405096757,0.12339672238346343,-0.10903747969310579}, {0.3022813561234955,-0.024881690546276862,0.06771403309177028,0.0009395937627441363,-0.0012785189282877262,-1.0671027269943336e-05,9.71876468535445e-06,-2.0123140548204047e-08}, {0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,}}},
  {.duration = 0.55, .p = {{0.1500000000687584,-0.12366905612177849,-0.594722347868441,0.5084822468775191,0.35024291353796916,-0.3779551670922615,-0.030993486696122376,0.07576798641159813}, {-7.039928298561939e-11,0.42840009257875555,-0.3532404208862144,-0.533157818385911,0.48806215190950436,0.19417616261494053,-0.2723172772132073,0.059875896856828974}, {0.30911898543159044,0.04960362137961011,0.06693941536418026,-0.0018731570659139287,-0.0012638933619877498,2.1194700149092597e-05,9.60648002352551e-06,-1.9002499360993986e-07}, {0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,}}},
  {.duration = 0.55, .p = {{3.6419711080421585e-11,-0.2509509341878004,0.28840823864950105,0.2721199858390575,-0.3997520324183086,-0.05878118540823653,0.21177485599121348,-0.0663789245795264}, {0.08786796571845405,-0.10097564726160337,-0.3342824466477831,0.41499174957660356,0.14984295740004752,-0.3147779789736173,0.045020724305445986,0.04136242930126668}, {0.35622417940547646,0.12070852848236742,0.061602987101403175,-0.004558255327930595,-0.0011631355987503336,5.1616188097919715e-05,8.839307227640228e-06,-3.46842188288027e-07}, {0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,}}},
  {.duration = 0.55, .p = {{-0.040192378935675475,0.07140085425594436,0.13444405880828958,-0.29325853912678074,0.004389222125442104,0.2288780962570675,-0.09946566658297257,-0.008165490863377235}, {3.342506343117202e-12,-0.11478929698434916,0.2039233240328519,0.07188800514013687,-0.28400040903603785,0.047455849220319694,0.1384729396693574,-0.06364285941067112}, {0.4403867969208466,0.1835873488493235,0.05206841707153454,-0.006932716025413217,-0.0009831120388661323,7.85200125196987e-05,7.469934672375333e-06,-4.801436347075494e-07}, {0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,}}},
  {.duration = 0.55, .p = {{2.657819140362752e-11,0.029194371526024346,-0.10554318434787024,0.05389264510610344,0.14869556371716416,-0.11729506325396347,-0.057406932212603616,0.05185415937314559}, {-0.01022225217203592,0.036960150373509054,0.00882584023135473,-0.15157853263098095,0.10194295680128089,0.12610945422193115,-0.12863100760957105,0.021560512581067035}, {0.5558712946793309,0.23395499478462367,0.03898547046555688,-0.008834723582265662,-0.0007360910302886222,0.00010007288325451688,5.591396797932501e-06,-5.806568866668146e-07}, {0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,}}},
};

static float sequenceTime(struct poly4d sequence[], int count) {
  float totalDuration = 0.0f;

  for (int i = 0; i < count; i++) {
    totalDuration += sequence[i].duration;
  }

  return totalDuration;
}

static float getX() { return logGetFloat(logIdStateEstimateX); }
static float getY() { return logGetFloat(logIdStateEstimateY); }
static float getZ() { return logGetFloat(logIdStateEstimateZ); }
static float getVarPX() { return logGetFloat(logIdKalmanVarPX); }
static float getVarPY() { return logGetFloat(logIdKalmanVarPY); }
static float getVarPZ() { return logGetFloat(logIdKalmanVarPZ); }
static bool isBatLow() { return logGetInt(logIdPmState) == lowPower; }
static bool isCharging() { return logGetInt(logIdPmState) == charging; }
static bool isLighthouseAvailable() { return logGetFloat(logIdlighthouseEstBs0Rt) >= 0.0f || logGetFloat(logIdlighthouseEstBs1Rt) >= 0.0f; }



#ifdef USE_MELLINGER
static void enableMellingerController() { paramSetInt(paramIdStabilizerController, ControllerTypeMellinger); }
#endif
static void enableHighlevelCommander() { paramSetInt(paramIdCommanderEnHighLevel, 1); }

static void defineTrajectory() {
  const uint32_t polyCount = sizeof(sequence) / sizeof(struct poly4d);
  trajectoryDurationMs = 1000 * sequenceTime(sequence, polyCount);
  crtpCommanderHighLevelWriteTrajectory(0, sizeof(sequence), (uint8_t*)sequence);
  crtpCommanderHighLevelDefineTrajectory(trajectoryId, CRTP_CHL_TRAJECTORY_TYPE_POLY4D, 0, polyCount);
}

static void defineLedSequence() {
  ledseqRegisterSequence(&seq_lock);
}

void appMain() {
  if (isInit) {
    return;
  }

  DEBUG_PRINT("This is a demo app\n");

  // Get log and param ids
  logIdStateEstimateX = logGetVarId("stateEstimate", "x");
  logIdStateEstimateY = logGetVarId("stateEstimate", "y");
  logIdStateEstimateZ = logGetVarId("stateEstimate", "z");
  logIdKalmanVarPX = logGetVarId("kalman", "varPX");
  logIdKalmanVarPY = logGetVarId("kalman", "varPY");
  logIdKalmanVarPZ = logGetVarId("kalman", "varPZ");
  logIdPmState = logGetVarId("pm", "state");
  logIdlighthouseEstBs0Rt = logGetVarId("lighthouse", "estBs0Rt");
  logIdlighthouseEstBs1Rt = logGetVarId("lighthouse", "estBs1Rt");

  paramIdStabilizerController = paramGetVarId("stabilizer", "controller");
  paramIdCommanderEnHighLevel = paramGetVarId("commander", "enHighLevel");
  paramIdLighthouseMethod = paramGetVarId("lighthouse", "method");


  timer = xTimerCreate("AppTimer", M2T(20), pdTRUE, NULL, appTimer);
  xTimerStart(timer, 20);

  pinMode(DECK_GPIO_IO3, INPUT_PULLUP);

  #ifdef USE_MELLINGER
    enableMellingerController();
  #endif

  enableHighlevelCommander();
  defineTrajectory();
  defineLedSequence();
  resetLockData();

  isInit = true;
}

static void appTimer(xTimerHandle timer) {
  uint32_t previous = now;
  now = xTaskGetTickCount();
  uint32_t delta = now - previous;

  if(supervisorIsTumbled()) {
    state = STATE_CRASHED;
  }

  if (isBatLow()) {
    terminateTrajectoryAndLand = true;
  }

  switch(state) {
    case STATE_IDLE:
      DEBUG_PRINT("Let's go! Waiting for position lock...\n");
      state = STATE_WAIT_FOR_POSITION_LOCK;
      break;
    case STATE_WAIT_FOR_POSITION_LOCK:
      if (hasLock()) {
        DEBUG_PRINT("Position lock acquired, ready for take off..\n");
        ledseqRun(&seq_lock);
        state = STATE_WAIT_FOR_TAKE_OFF;
      }
      break;
    case STATE_WAIT_FOR_TAKE_OFF:
      trajectoryStartTime = 0;
      if (takeOffWhenReady) {
        takeOffWhenReady = false;
        DEBUG_PRINT("Taking off!\n");

        padX = getX();
        padY = getY();
        padZ = getZ();
        DEBUG_PRINT("Base position: (%f, %f, %f)\n", (double)padX, (double)padY, (double)padZ);

        terminateTrajectoryAndLand = false;
        crtpCommanderHighLevelTakeoff(padZ + TAKE_OFF_HEIGHT, 1.0);
        state = STATE_TAKING_OFF;
      }
      break;
    case STATE_TAKING_OFF:
      if (crtpCommanderHighLevelIsTrajectoryFinished()) {
        DEBUG_PRINT("Hovering, waiting for command to start\n");
        ledseqStop(&seq_lock);
        state = STATE_HOVERING;

      }
      flightTime += delta;
      break;
    case STATE_HOVERING:
      if (terminateTrajectoryAndLand) {
          terminateTrajectoryAndLand = false;
          DEBUG_PRINT("Terminating hovering\n");
          state = STATE_GOING_TO_PAD;
      } else {
        if (goToInitialPositionWhenReady >= 0.0f) {
          float delayMs = goToInitialPositionWhenReady * trajectoryDurationMs;
          timeWhenToGoToInitialPosition = now + delayMs;
          trajectoryStartTime = now + delayMs;
          goToInitialPositionWhenReady = -1.0f;
          DEBUG_PRINT("Waiting to go to initial position for %d ms\n", (int)delayMs);
          state = STATE_WAITING_TO_GO_TO_INITIAL_POSITION;
        }
      }
      flightTime += delta;
      break;
    case STATE_WAITING_TO_GO_TO_INITIAL_POSITION:
      if (now >= timeWhenToGoToInitialPosition) {
        DEBUG_PRINT("Going to initial position\n");
        crtpCommanderHighLevelGoTo(sequence[0].p[0][0] + trajecory_center_offset_x, sequence[0].p[1][0] + trajecory_center_offset_y, sequence[0].p[2][0] + trajecory_center_offset_z, sequence[0].p[3][0], DURATION_TO_INITIAL_POSITION, false);
        state = STATE_GOING_TO_INITIAL_POSITION;
      }
      flightTime += delta;
      break;
    case STATE_GOING_TO_INITIAL_POSITION:
      currentProgressInTrajectory = (now - trajectoryStartTime) / trajectoryDurationMs;

      if (crtpCommanderHighLevelIsTrajectoryFinished()) {
        DEBUG_PRINT("At initial position, starting trajectory...\n");
        crtpCommanderHighLevelStartTrajectory(trajectoryId, SEQUENCE_SPEED, true, false, false);
        remainingTrajectories = trajectoryCount - 1;
        state = STATE_RUNNING_TRAJECTORY;
      }
      flightTime += delta;
      break;
    case STATE_RUNNING_TRAJECTORY:
      currentProgressInTrajectory = (now - trajectoryStartTime) / trajectoryDurationMs;

      if (crtpCommanderHighLevelIsTrajectoryFinished()) {
        if (terminateTrajectoryAndLand || (remainingTrajectories == 0)) {
          terminateTrajectoryAndLand = false;
          DEBUG_PRINT("Terminating trajectory, going to pad...\n");
          float timeToPadPosition = 2.0;
          crtpCommanderHighLevelGoTo(padX, padY, padZ + LANDING_HEIGHT, 0.0, timeToPadPosition, false);
          currentProgressInTrajectory = NO_PROGRESS;
          state = STATE_GOING_TO_PAD;
        } else {
          if (remainingTrajectories > 0) {
            DEBUG_PRINT("Trajectory finished, restarting...\n");
            crtpCommanderHighLevelStartTrajectory(trajectoryId, SEQUENCE_SPEED, true, false, false);
          }
          remainingTrajectories--;
        }
      }
      flightTime += delta;
      break;
    case STATE_GOING_TO_PAD:
      if (crtpCommanderHighLevelIsTrajectoryFinished()) {
        DEBUG_PRINT("Over pad, stabalizing position\n");
        stabilizeEndTime = now + 5000;
        state = STATE_WAITING_AT_PAD;
      }
      flightTime += delta;
      break;
    case STATE_WAITING_AT_PAD:
      if (now > stabilizeEndTime || ((fabs(padX - getX()) < MAX_PAD_ERR) && (fabs(padY - getY()) < MAX_PAD_ERR))) {
        if (now > stabilizeEndTime) {
          DEBUG_PRINT("Warning: timeout!\n");
        }

        DEBUG_PRINT("Landing...\n");
        crtpCommanderHighLevelLand(padZ, 1.0);
        state = STATE_LANDING;
      }
      flightTime += delta;
      break;
    case STATE_LANDING:
      if (crtpCommanderHighLevelIsTrajectoryFinished()) {
        DEBUG_PRINT("Landed. Feed me!\n");
        crtpCommanderHighLevelStop();
        landingTimeCheckCharge = now + 3000;
        state = STATE_CHECK_CHARGING;
      }
      flightTime += delta;
      break;
    case STATE_CHECK_CHARGING:
      if (now > landingTimeCheckCharge) {
        DEBUG_PRINT("isCharging: %d\n", isCharging());
        if (isCharging()) {
          ledseqRun(&seq_lock);
          state = STATE_WAIT_FOR_TAKE_OFF;
        } else {
          DEBUG_PRINT("Not charging. Try to reposition on pad.\n");
          crtpCommanderHighLevelTakeoff(padZ + LANDING_HEIGHT, 1.0);
          state = STATE_REPOSITION_ON_PAD;
        }
      }
      break;
    case STATE_REPOSITION_ON_PAD:
      if (crtpCommanderHighLevelIsTrajectoryFinished()) {
        DEBUG_PRINT("Over pad, stabalizing position\n");
        crtpCommanderHighLevelGoTo(padX, padY, padZ + LANDING_HEIGHT, 0.0, 1.5, false);
        state = STATE_GOING_TO_PAD;
      }
      flightTime += delta;
      break;
    case STATE_CRASHED:
      crtpCommanderHighLevelStop();
      break;
    default:
      break;
  }
}


static bool hasLock() {
  bool result = false;

  // Store current state
  lockData[lockWriteIndex][0] = getVarPX();
  lockData[lockWriteIndex][1] = getVarPY();
  lockData[lockWriteIndex][2] = getVarPZ();

  lockWriteIndex++;
  if (lockWriteIndex >= LOCK_LENGTH) {
    lockWriteIndex = 0;
  }

  // Check if we have a lock
  int count = 0;

  float lXMax = FLT_MIN;
  float lYMax = FLT_MIN;
  float lZMax = FLT_MIN;

  float lXMin = FLT_MAX;
  float lYMin = FLT_MAX;
  float lZMin = FLT_MAX;

  for (int i = 0; i < LOCK_LENGTH; i++) {
    if (lockData[i][0] != FLT_MAX) {
      count++;

      lXMax = fmaxf(lXMax, lockData[i][0]);
      lYMax = fmaxf(lYMax, lockData[i][1]);
      lZMax = fmaxf(lZMax, lockData[i][2]);

      lXMin = fminf(lXMax, lockData[i][0]);
      lYMin = fminf(lYMin, lockData[i][1]);
      lZMin = fminf(lZMin, lockData[i][2]);
    }
  }

  result =
    (count >= LOCK_LENGTH) &&
    ((lXMax - lXMin) < LOCK_THRESHOLD) &&
    ((lYMax - lYMin) < LOCK_THRESHOLD) &&
    ((lZMax - lZMin) < LOCK_THRESHOLD &&
    isLighthouseAvailable() &&  // Make sure we have a deck and the Lighthouses are powered
    sensorsAreCalibrated());

  return result;
}

static void resetLockData() {
    lockWriteIndex = 0;
    for (uint32_t i = 0; i < LOCK_LENGTH; i++) {
      lockData[i][0] = FLT_MAX;
      lockData[i][1] = FLT_MAX;
      lockData[i][2] = FLT_MAX;
    }
}

PARAM_GROUP_START(app)
  PARAM_ADD(PARAM_UINT8, takeoff, &takeOffWhenReady)
  PARAM_ADD(PARAM_FLOAT, start, &goToInitialPositionWhenReady)
  PARAM_ADD(PARAM_UINT8, stop, &terminateTrajectoryAndLand)
  PARAM_ADD(PARAM_FLOAT, offsx, &trajecory_center_offset_x)
  PARAM_ADD(PARAM_FLOAT, offsy, &trajecory_center_offset_y)
  PARAM_ADD(PARAM_FLOAT, offsz, &trajecory_center_offset_z)
  PARAM_ADD(PARAM_UINT8, trajcount, &trajectoryCount)
PARAM_GROUP_STOP(app)

LOG_GROUP_START(app)
  LOG_ADD(LOG_UINT8, state, &state)
  LOG_ADD(LOG_FLOAT, prgr, &currentProgressInTrajectory)
  LOG_ADD(LOG_UINT32, uptime, &now)
  LOG_ADD(LOG_UINT32, flighttime, &flightTime)
LOG_GROUP_STOP(app)
