#include "vision.h"
#include "table.h"
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>

double matchStartTime;
int matchState = MATCH_ENDED;
volatile int sendStartPacket = 0; // flag to have a start packet sent ASAP
volatile int sendStopPacket = 0;
volatile int sendPositionPacket = 0;

#define NUM_OBJECTS 32
game_data gameData;
game_data serialGameData;

char *teams[MAX_ROBOT_ID+1];
int scores[MAX_ROBOT_ID+1];

pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t serial_condition = PTHREAD_COND_INITIALIZER;
int sightings[MAX_ROBOT_ID+1];
board_coord robots[MAX_ROBOT_ID+1];

// For a square playing field, these would be X_MIN, X_MAX, etc,
// but since this is hexagonal, we use different (interior) points
float bounds[4] = {-1024, 1024, -1773, 1773};

#define SHOW_FILTERED_OUTPUT 1

const char *WND_MAIN = "6.270 Vision System";
const char *WND_FILTERED = "Filtered Video";
const char *WND_CONTROLS = "Controls";
const char *WND_FILTERED_SQUARES = "Square Detection";
const char *TRK_THRESHOLD = "Threshold";
const char *TRK_TOLERANCE = "Side length tolerance";
const char *TRK_MIN_AREA = "Min square area";
const char *TRK_MAX_AREA = "Max square area";
const char *TRK_MIN_BALL_DIM = "Min ball dimension";
const char *TRK_MAX_BALL_DIM = "Max ball dimension";
const char *TRK_BALL_THRESHOLD = "Ball brightness threshold";

const char *TRK_CANNY_THRESHOLD = "Canny upper threshold";
const char *TRK_HOUGH_VOTES = "Minimum Hough votes";

enum {
    PICK_PROJECTION_CORNERS,
    PICK_SAMPLE_CORNERS,
    PICK_EXCLUDE_CORNERS
} mouseOperation = PICK_PROJECTION_CORNERS;
int nextMousePoint = 4;

const char *mouseCornerLabel[] = {"TOP LEFT", "TOP RIGHT", "BOTTOM RIGHT", "BOTTOM LEFT"};
const char *mouseOperationLabel[] = {"Init Projection", "Sample Colors", "Exclude Quad"};

int threshold = 100;
int randomGoalSeed = 18022;
int side_tolerance = 50;
int min_area = 800; // ~ square of (fraction of frame width in 1/1000s)
int max_area = 5800; // will be corrected for resolution
int canny_threshold = 20;
int hough_votes = 80;

int min_ball_dim = 6;
int max_ball_dim = 16;
int ball_threshold = 90;

CvFont font, titleFonts[4], hudFont;
CvMemStorage *storage;
CvCapture *capture;
float frameWidth, frameHeight;
const float displayWidth = 1024, displayHeight = 768;

int fd_tx; // file descriptor of TX happyboard serial
int fd_pf; // file descriptor of playing field happyboard serial

void music() {
    system("/home/sixtwoseventy/music-ctl.py &");
}

void music_off() {
    system("/home/sixtwoseventy/music-ctl.py shutup &");
}

void music_fade() {
    system("/home/sixtwoseventy/music-ctl.py fade &");
}


int cvPrintf(IplImage *img, CvFont *font, CvPoint pt, CvScalar color, const char *format, ...);

// For projections: 
// Frame is the video frame from the camera (i.e. 640x480 coords)
// Physical is the robot coordinates (i.e. 4096x4096)
// Display is the display coordinates
CvMat *projection = 0; // maps from frame coords to physical coords
CvMat *displayMatrix = 0; // maps from physical coords to display coords
CvMat *invProjection = 0; // maps from physical coords to frame coords

int warpDisplay = 0, showFPS = 0;
int showPhotoFinish = 0;

bool showFilteredSquares = 0;

CvPoint2D32f projectionPoints[4];
CvPoint2D32f sampleCorners[4];
int nextExclude = 0;
CvPoint2D32f excludeCorners[10][4];
int sampleColors = 0;

int thisBoard = 0;
char boardLetter = '*';

void computeDisplayMatrix() {
    if (displayMatrix)
        cvReleaseMat(&displayMatrix);
    if (warpDisplay) {
        CvMat *M = cvCreateMat(3,3, CV_32FC1);

        CvPoint2D32f src[4] = {cvPoint2D32f(X_MIN,Y_MAX),cvPoint2D32f(X_MAX,Y_MAX),cvPoint2D32f(X_MAX,Y_MIN),cvPoint2D32f(X_MIN,Y_MIN)};
        //CvPoint2D32f dst[4] = {cvPoint2D32f(0,0),cvPoint2D32f(frameHeight,0),cvPoint2D32f(frameHeight,frameHeight),cvPoint2D32f(0,frameHeight)};
        CvPoint2D32f dst[4] = {cvPoint2D32f(0,0),cvPoint2D32f(displayHeight,0),cvPoint2D32f(displayHeight,displayHeight),cvPoint2D32f(0,displayHeight)};

        cvGetPerspectiveTransform(src, dst, M);

        displayMatrix = M;
    } else
        displayMatrix = cvCloneMat(invProjection);
}

void saveExclusions() {
    CvMat matrix = cvMat(10,4,CV_32FC2,excludeCorners);
    char buf[256];
    sprintf(buf, "Exclusions%c.xml", boardLetter);
    cvSave( buf, &matrix, 0, 0, cvAttrList(0, 0) );
}

void mouseHandler(int event, int x, int y, int flags, void *param) {
    CvPoint2D32f point = cvPoint2D32f(x,y);
    if (event == CV_EVENT_LBUTTONDOWN && nextMousePoint < 4) {
        CvPoint2D32f *arr;
        if (mouseOperation == PICK_PROJECTION_CORNERS)
            arr = projectionPoints;
        else if (mouseOperation == PICK_SAMPLE_CORNERS)
            arr = sampleCorners;
        else if (mouseOperation == PICK_EXCLUDE_CORNERS)
            arr = excludeCorners[nextExclude];
        arr[nextMousePoint++] = point;
        if (nextMousePoint == 4) {
            switch (mouseOperation) {
                case PICK_PROJECTION_CORNERS:
                    projection_init(&projection, &invProjection, projectionPoints, bounds);
                    computeDisplayMatrix();
                    CvMat matrix = cvMat(4,1,CV_32FC2,projectionPoints);
                    char buf[256];
                    sprintf(buf, "Projection%c.xml", boardLetter);
                    cvSave( buf, &matrix, 0, 0, cvAttrList(0, 0) );
                    printf("project init %s\n", (projection && invProjection) ? "succeeded" : "failed");
                    break;
                case PICK_SAMPLE_CORNERS:
                    sampleColors = 1;
                    break;
                case PICK_EXCLUDE_CORNERS:
                    nextExclude++;
                    saveExclusions();
                    break;
            }
        }
    }
}

IplImage *filter_image( IplImage *img ) {
    CvSize sz = cvSize( img->width & -2, img->height & -2 );

    //IplImage *timg = cvCloneImage( img ); // make a copy of input image
    //IplImage *pyr = cvCreateImage( cvSize(sz.width/2, sz.height/2), 8, 3 );
    IplImage *tgray;

    // select the maximum ROI in the image
    // with the width and height divisible by 2
    cvSetImageROI( img, cvRect( 0, 0, sz.width, sz.height ));

    // down-scale and upscale the image to filter out the noise
    //cvPyrDown( timg, pyr, 7 );
    //cvPyrUp( pyr, timg, 7 );
    tgray = cvCreateImage( sz, 8, 1 );

    cvCvtColor(img, tgray, CV_BGR2GRAY);
    //cvReleaseImage( &pyr );
    //cvReleaseImage( &timg );

    return tgray;
}



double getObjectDistance(board_coord a, board_coord b){
    if (a.id != 0xFF || b.id != 0xFF){
        return INFINITY;
    }
    return dist_sq(cvPoint(a.x,a.y), cvPoint(b.x,b.y));
}

typedef struct {
    int prevIdx;
    int curIdx;
    double distance;
} obj_dist;

int compareDists(const void *a, const void *b){
    obj_dist *A = (obj_dist*)a;
    obj_dist *B = (obj_dist*)b;

    if (A->distance < B->distance)
        return -1;
    else if (A->distance == B->distance)
        return 0;
    else 
        return 1;
}


// returns sequence of squares detected on the image.
// the sequence is stored in the shared memory storage
CvSeq *findCandidateSquares(IplImage *tgray) {
    CvSeq *contours;
    int i;
    CvSize sz = cvSize( tgray->width & -2, tgray->height & -2 );
    IplImage *gray = cvCreateImage( sz, 8, 1 );
    CvSeq *result;
    double s, t;
    // create empty sequence that will contain points -
    // 4 points per square (the square's vertices)
    CvSeq *squares = cvCreateSeq( 0, sizeof(CvSeq), sizeof(CvPoint), storage );

    cvThreshold( tgray, gray, threshold, 255, CV_THRESH_BINARY );
    //cvAdaptiveThreshold(tgray, gray, 255, CV_ADAPTIVE_THRESH_GAUSSIAN_C, CV_THRESH_BINARY, 13, threshold);

    if (showFilteredSquares) {
        cvShowImage(WND_FILTERED_SQUARES, gray);
    }

    // find contours and store them all as a list
    cvFindContours( gray, storage, &contours, sizeof(CvContour),
            CV_RETR_LIST, CV_CHAIN_APPROX_SIMPLE, cvPoint(0,0) );

    // test each contour
    while( contours ) {

        // approximate contour with accuracy proportional
        // to the contour perimeter
        result = cvApproxPoly( contours, sizeof(CvContour), storage,
                CV_POLY_APPROX_DP, cvContourPerimeter(contours)*0.02, 0 );
        // square contours should have 4 vertices after approximation
        // relatively large area (to filter out noisy contours)
        // and be convex.
        // Note: absolute value of an area is used because
        // area may be positive or negative - in accordance with the
        // contour orientation
        if( result->total == 4 &&
                -cvContourArea(result,CV_WHOLE_SEQ,1) > min_area &&
                -cvContourArea(result,CV_WHOLE_SEQ,1) < max_area &&
                cvCheckContourConvexity(result)) {
            s = 0;

            for( i = 2; i < 5; i++ ) {
                // find minimum angle between joint
                // edges (maximum of cosine)
                t = fabs(cosAngle((CvPoint*)cvGetSeqElem( result, i ),
                            (CvPoint*)cvGetSeqElem( result, i-2 ),
                            (CvPoint*)cvGetSeqElem( result, i-1 )));
                s = s > t ? s : t;
            }

            // if cosines of all angles are small (angles are ~90 degrees)
            if( s < 0.3 ) {
                CvPoint pt[4];
                for (i=0; i<4; i++)
                    pt[i] = *(CvPoint*)cvGetSeqElem(result, 3-i);

                // calculate the length of each side
                double side_len[4];
                side_len[0] = dist_sq(pt[0],pt[1]);
                side_len[1] = dist_sq(pt[1],pt[2]);
                side_len[2] = dist_sq(pt[2],pt[3]);
                side_len[3] = dist_sq(pt[3],pt[0]);

                double tolerance = (double)side_tolerance / 100.;
                // check to make sure all sides are approx. the same length as side 0
                if (fabs(side_len[0] - side_len[1])/side_len[0] <= tolerance &&
                        fabs(side_len[0] - side_len[2])/side_len[0] <= tolerance &&
                        fabs(side_len[0] - side_len[3])/side_len[0] <= tolerance) {
                    // then write quandrange vertices to resultant sequence in clockwise order

                    for( i = 0; i < 4; i++ )
                        cvSeqPush( squares, &pt[i] );
                }
            }
        }


        // take the next contour
        contours = contours->h_next;
    }

    // release all the temporary images
    cvReleaseImage( &gray );
    return squares;
}

int cvPrintf(IplImage *img, CvFont *font, CvPoint pt, CvScalar color, const char *format, ...) {
    static char buffer[2048];
    va_list ap;
    int count;

    va_start(ap, format);
    count = vsnprintf(buffer, 2048, format, ap);
    va_end(ap);

    cvPutText(img, buffer, pt, font, color);
    return count;
}

void getBitSamplingTransform(CvPoint pt[4], CvMat **H);

void estimateReticleParams(CvMat *H, float *cx, float *cy, float *radius, float *theta) {
    CvPoint2D32f vf[3];
    CvMat m = cvMat(1, 3, CV_32FC2, vf);
    vf[0] = cvPoint2D32f(2.0, 2.0);
    vf[1] = cvPoint2D32f(3.0, 2.0);
    vf[2] = cvPoint2D32f(2.0, 3.0);
    cvPerspectiveTransform(&m, &m, H);
    *cx = vf[0].x;
    *cy = vf[0].y;
    float a = sqrt((vf[1].x-vf[0].x)*(vf[1].x-vf[0].x) + (vf[1].y-vf[0].y)*(vf[1].y-vf[0].y));
    float b = sqrt((vf[2].x-vf[0].x)*(vf[2].x-vf[0].x) + (vf[2].y-vf[0].y)*(vf[2].y-vf[0].y));
    *radius = sqrt(a*b)*5;

    vf[0] = cvPoint2D32f(0.0, 0.0);
    vf[1] = cvPoint2D32f(cos(*theta), sin(*theta));
    cvPerspectiveTransform(&m, &m, displayMatrix);
    *theta = -atan2(vf[1].y-vf[0].y, vf[1].x-vf[0].x);
}

void drawChevron(IplImage *out, float theta, float alpha, float dtheta, float t, int big, float cx, float cy, float radius) {
    CvPoint v[4];
    CvPoint2D32f vf[4];
    CvMat m = cvMat(1, 4, CV_32FC2, vf);
    CvPoint *p[1];
    int count[1];

    if (big)
        dtheta = acos((1.0/alpha) * (1 + t/(2.0*radius)));
    else
        alpha -= t/(2.0*radius);

    v[0] = cvPoint(8*(cx + radius*      cos(theta+dtheta)), 8*(cy - radius*      sin(theta+dtheta)));
    v[1] = cvPoint(8*(cx + radius*alpha*cos(theta       )), 8*(cy - radius*alpha*sin(theta       )));
    v[2] = cvPoint(8*(cx + radius*      cos(theta-dtheta)), 8*(cy - radius*      sin(theta-dtheta)));
    v[3] = cvPoint(8*(cx + radius*      cos(theta       )), 8*(cy - radius*      sin(theta       )));
    p[0] = &v[0];
    count[0] = 4;
    cvFillPoly(out, p, count, 1, CV_RGB(0,255,255), CV_AA, 3);
}

void drawCallout(IplImage *out, float cx, float cy, float radius, int id) {
    CvPoint v[4];
    CvPoint2D32f vf[4];
    CvMat m = cvMat(1, 4, CV_32FC2, vf);
    CvPoint *p[1];
    int count[1];

    char buf[256];

    sprintf(buf, "Team 00");
    CvSize maxTextSize;
    int baseline;
    cvGetTextSize(buf, &font, &maxTextSize, &baseline);

    sprintf(buf, "Team %i", id);
    CvSize textSize;
    cvGetTextSize(buf, &font, &textSize, &baseline);

    float y;
    float flipY = (warpDisplay ? displayHeight : frameHeight) - 40;
    int down = cy+radius+20+textSize.height < flipY;

    if (down)
        y = cy+radius+20+textSize.height;
    else
        y = cy-radius-20;

    cvPrintf(out, &font, cvPoint(cx-textSize.width/2.0, y-baseline), CV_RGB(0,255,255), "Team %i", id);

    float lx = cx-maxTextSize.width/2-10;
    v[0] = cvPoint(8*(cx-radius), 8*cy);
    v[1] = cvPoint(8*lx, 8*cy);
    v[2] = cvPoint(8*lx, 8*y);
    v[3] = cvPoint(8*(cx+textSize.width/2), 8*y);

    p[0] = &v[0];
    count[0] = 4;

    cvPolyLine(out, p, count, 1, 0, CV_RGB(0,255,255), 2, CV_AA, 3);
}

void drawSquare(IplImage *out, IplImage *gray, CvPoint pt[4], CvPoint2D32f bit_pt_true[16], int id, CvPoint2D32f orientationHandle, float theta) {
    // draw the square as a closed polyline
    CvPoint v[20];
    CvPoint2D32f vf[20];
    CvMat m;
    CvPoint *p[10];
    int count[10];

    float l = -1.0, r=5.0, w=4.0;         // w is size of circle exclusion zone
    CvMat *H = cvCreateMat(3,3,CV_32FC1); // tag coords to display coords
    CvMat *A = 0;                         // tag coords to frame coords
    CvMat *B = cvCreateMat(3,3,CV_32FC1); // frame coords to display coords

    getBitSamplingTransform(pt, &A);
    cvMatMul(displayMatrix, projection, B);
    cvMatMul(B, A, H);
    cvReleaseMat(&B);

    //black out square area in original grayscale image since it has been processed
    vf[0] = cvPoint2D32f(l-w, l-w);
    vf[1] = cvPoint2D32f(r+w, l-w);
    vf[2] = cvPoint2D32f(r+w, r+w);
    vf[3] = cvPoint2D32f(l-w, r+w);
    m = cvMat(1, 4, CV_32FC2, vf);
    cvPerspectiveTransform(&m, &m, A);

    for (int i=0; i<4; i++)
        v[i] = cvPoint(vf[i].x*8, vf[i].y*8);
    p[0] = v;
    count[0] = 4;
    cvFillPoly(gray, p, count, 1, CV_RGB(0,0,0), 8, 3);

    cvReleaseMat(&A);

    if (id == -1) {
        cvReleaseMat(&H);
        return;
    }

    vf[0] = cvPoint2D32f(l, l);
    vf[1] = cvPoint2D32f(r, l);
    vf[2] = cvPoint2D32f(r, r);
    vf[3] = cvPoint2D32f(l, r);
    m = cvMat(1, 4, CV_32FC2, vf);
    cvPerspectiveTransform(&m, &m, H);

    for (int i=0; i<4; i++)
        v[i] = cvPoint(vf[i].x*8, vf[i].y*8);
    p[0] = v;
    count[0] = 4;

    cvFillPoly(out, p, count, 1, CV_RGB(0,0,0), CV_AA, 3);

    for (int i=0; i<5; i++) {
        vf[2*i+ 0] = cvPoint2D32f(i,l);
        vf[2*i+ 1] = cvPoint2D32f(i,r);
        vf[2*i+10] = cvPoint2D32f(l,i);
        vf[2*i+11] = cvPoint2D32f(r,i);
        p[i+0] = &v[2*i+0];
        count[i+0] = 2;
        p[i+5] = &v[2*i+10];
        count[i+5] = 2;
    }
    m = cvMat(1, 20, CV_32FC2, vf);
    cvPerspectiveTransform(&m, &m, H);
    for (int i=0; i<20; i++)
        v[i] = cvPoint(vf[i].x*8, vf[i].y*8);
    cvPolyLine(out, p, count, 10, 0, CV_RGB(0,255,255), 1, CV_AA, 3);

    float cx, cy, radius;
    estimateReticleParams(H, &cx, &cy, &radius, &theta);

    float t = 2.0;
    cvCircle(out, cvPoint(cx*8,cy*8), radius*8, CV_RGB(0,255,255), t, CV_AA, 3);

    drawChevron(out, theta, 1.3, 0., t, 1, cx, cy, radius);
    drawChevron(out, theta+M_PI/4,   0.9, .1, t, 0, cx, cy, radius);
    drawChevron(out, theta-M_PI/4,   0.9, .1, t, 0, cx, cy, radius);
    drawChevron(out, theta+3*M_PI/4, 0.9, .1, t, 0, cx, cy, radius);
    drawChevron(out, theta-3*M_PI/4, 0.9, .1, t, 0, cx, cy, radius);

    for (int i=0; i<4; i++)
        drawChevron(out, theta+i*M_PI/2, 0.5, .01, t, 0, cx, cy, radius);

    cvReleaseMat(&H);

    drawCallout(out, cx, cy, radius, id);
}

void getBitSamplingTransform(CvPoint pt[4], CvMat **H) {
    const float l = -.5, r = 4.5;
    CvPoint2D32f src[4] = {{l,l},{r,l},{r,r},{l,r}};
    CvPoint2D32f dst[4] = {{pt[0].x,pt[0].y},{pt[1].x,pt[1].y},{pt[2].x,pt[2].y},{pt[3].x,pt[3].y}};
    *H = cvCreateMat(3,3,CV_32FC1);
    *H = cvGetPerspectiveTransform(src, dst, *H);
}

int getOrientationFromBits(int bit_raw[16], int *orientation) {
    int corner[4];
    corner[0] = bit_raw[0];
    corner[1] = bit_raw[3];
    corner[2] = bit_raw[15];
    corner[3] = bit_raw[12];

    // registration corner is the white corner whose clockwise neighbor is black
    *orientation = -1;
    for (int j=0; j<4; j++) {
        if(corner[j] && !corner[(j+1) % 4]) {
            if (*orientation != -1) { // corner is ambiguous
                *orientation = -1;
                break;
            }
            *orientation = j;
        }
    }
    return *orientation != -1;
}

int getIDFromBits(int bit_true[16], int *id) {
    *id = (bit_true[5] << 0) + (bit_true[6] << 1) + (bit_true[9] << 2) + (bit_true[10] << 3);

    /* 
    *id = (bit_true[5] << 0) + (bit_true[6] << 1) + (bit_true[9] << 2) + (bit_true[10] << 3) +
        (bit_true[1] << 4) + (bit_true[2] << 5) + (bit_true[4] << 6) + (bit_true[7] << 7) +
        (bit_true[8] << 8) + (bit_true[11] << 9) + (bit_true[13] << 10) + (bit_true[14] << 11) +
        ((bit_true[12] + bit_true[15]) << 12);
    */
    return 1;
}

void rotateBitsToOrientation(CvPoint2D32f bit_pt_raw[16], int bit_raw[16], int orientation, CvPoint2D32f bit_pt_true[16], int bit_true[16]) {
    // shift indices so that orientation -> 0
    for (int j=0; j<16; j++) {
        int x = j%4, y=j/4;
        int xp, yp;
        switch (orientation) {
            case 0:
                xp = x; yp = y;
                break;
            case 1:
                xp = 3-y; yp = x;
                break;
            case 2:
                xp = 3-x; yp = 3-y;
                break;
            case 3:
                xp = y; yp = 3-x;
                break;
        }
        int j_raw = xp + yp*4;
        bit_pt_true[j] = bit_pt_raw[j_raw];
        bit_true[j] = bit_raw[j_raw];
    }
}

int readPattern(IplImage *img, CvPoint pt[4], CvPoint2D32f bit_pt_true[16], int *id) {
    CvPoint2D32f bit_pt_raw[16];
    int bit_raw[16], bit_true[16];

    CvMat *H;
    getBitSamplingTransform(pt, &H);
    //calculate the coordinates of each bit
    for (int j=0; j<16; j++)
        bit_pt_raw[j] = cvPoint2D32f(.5 + j%4, .5 + j/4);
    CvMat pts = cvMat(1, 16, CV_32FC2, bit_pt_raw);
    cvPerspectiveTransform(&pts, &pts, H);
    for (int j=0; j<16; j++) {
        CvScalar sample = cvGet2D(img, bit_pt_raw[j].y, bit_pt_raw[j].x);
        bit_raw[j] =  ((sample.val[0] + sample.val[1] + sample.val[2])/3. > threshold);
    }
    cvReleaseMat(&H);

    int num=0;
    for (int j=0; j<16; j++)
        num |= bit_raw[j]<<j;

    int orientation, dist;
    HAMMING_DECODE(num, id, &orientation, &dist);
    *id += 1; // 1 to 32
    //printf("%5d %2d %1d %1d\n", num, *id, orientation, dist);
    if (dist>3) return 0;
    rotateBitsToOrientation(bit_pt_raw, bit_raw, orientation, bit_pt_true, bit_true);
    return 1;
}

void getCenterFromBits(CvPoint2D32f bit_pt_true[16], CvPoint2D32f *trueCenter) {
    CvPoint2D32f rawCenter = cvPoint2D32f((bit_pt_true[0].x + bit_pt_true[3].x + bit_pt_true[15].x + bit_pt_true[12].x)/4,
            (bit_pt_true[0].y + bit_pt_true[3].y + bit_pt_true[15].y + bit_pt_true[12].y)/4);
    *trueCenter = project(projection, rawCenter);
}

float getThetaFromAffine(CvPoint2D32f bit_pt_true[16]) {
    const float l = 0.5, r = 3.5;
    CvPoint2D32f src[4] = {{l,l},{r,l},{r,r},{l,r}};
    CvPoint2D32f dst[4] = {bit_pt_true[0],bit_pt_true[3],bit_pt_true[15],bit_pt_true[12]};
    CvMat *A = cvCreateMat(2,3,CV_32FC1);

    CvMat srcM = cvMat(1, 4, CV_32FC2, src);
    CvMat dstM = cvMat(1, 4, CV_32FC2, dst);
    cvEstimateRigidTransform(&srcM, &dstM, A, 1);

    // use affine approximation and SVD to determine angle
    float A22_mat[2][2] = {{cvmGet(A, 0, 0),cvmGet(A, 0, 1)},{cvmGet(A, 1, 0),cvmGet(A, 1, 1)}};
    CvMat A22  = cvMat(2,2,CV_32FC1,A22_mat);
    float U_mat[2][2], W_mat[2][2], V_mat[2][2], R_mat[2][2];
    CvMat U = cvMat(2,2,CV_32FC1,U_mat);
    CvMat W = cvMat(2,2,CV_32FC1,W_mat);
    CvMat V = cvMat(2,2,CV_32FC1,V_mat);
    CvMat R = cvMat(2,2,CV_32FC1,R_mat);
    cvSVD(&A22, &W, &U, &V, CV_SVD_U_T|CV_SVD_V_T); // A = U D V^T
    cvTranspose(&U, &U);
    cvMatMulAdd(&U, &V, 0, &R);
    float theta = atan2(R_mat[1][0], R_mat[0][0]);
    cvReleaseMat(&A);
    return theta;
}

float getThetaFromExtension(CvPoint2D32f bit_pt_true[16], CvPoint2D32f trueCenter) {
    //to find the heading, "extend" the top and bottom edges 4x to the right and take
    //  the average endpoint, then project this and take the dx and dy in the projected
    //  space to find the angle it makes

    fiducial_t fiducial;
    fiducial.corners[0] = bit_pt_true[0];
    fiducial.corners[1] = bit_pt_true[3];
    fiducial.corners[2] = bit_pt_true[15];
    fiducial.corners[3] = bit_pt_true[12];

    int extended_top_x = fiducial.corners[TR].x + (fiducial.corners[TR].x - fiducial.corners[TL].x)*4;
    int extended_top_y = fiducial.corners[TR].y + (fiducial.corners[TR].y - fiducial.corners[TL].y)*4;

    int extended_bottom_x = fiducial.corners[BR].x + (fiducial.corners[BR].x - fiducial.corners[BL].x)*4;
    int extended_bottom_y = fiducial.corners[BR].y + (fiducial.corners[BR].y - fiducial.corners[BL].y)*4;

    int extended_avg_x = (extended_top_x+extended_bottom_x)/2;
    int extended_avg_y = (extended_top_y+extended_bottom_y)/2;

    //project into coordinate space
    CvPoint2D32f projected_extension = project(projection, cvPoint2D32f(extended_avg_x,extended_avg_y));

    //find the dx and dy with respect to the fiducial's center point
    float dx = ((float)projected_extension.x-(float)trueCenter.x);
    float dy = ((float)projected_extension.y-(float)trueCenter.y);

    return atan2(dy,dx);
}

void processRobotDetection(CvPoint2D32f trueCenter, float theta, int id, CvPoint2D32f *orientationHandle) {
    *orientationHandle = cvPoint2D32f(trueCenter.x + FOOT*cos(theta), trueCenter.y + FOOT*sin(theta));
    *orientationHandle = project(invProjection, *orientationHandle);

    sightings[id]+=2;

    int x = clamp(trueCenter.x, X_MIN, X_MAX);
    int y = clamp(trueCenter.y, Y_MIN, Y_MAX);
    int t = theta / CV_PI * 2048;
    //store robot coordinates
    robots[id].id = id;
    robots[id].x = x;
    robots[id].y = y;
    robots[id].theta = t; //change theta from +/- PI to +/-2048 (signed 12 bit int)
    robots[id].score = scores[id];

    if (0)
        printf("X: %04i, Y: %04i, theta: %04i, theta_act: %f, proj_x:%f, proj_y:%f \n", x, y, t, theta, orientationHandle->x, orientationHandle->y);
}

void centeredFitTitleText(IplImage *out, CvScalar color, float y, float w, char *buf) {
    CvSize textSize;
    int baseline;

    if (buf == 0 || strlen(buf) == 0) return;

    int i;
    for (i=0; i<4; i++) {
        cvGetTextSize(buf, &titleFonts[i], &textSize, &baseline);
        if (textSize.width<w)
            break;
    }
    if (i==4)
        i = 3;
    cvPutText(out, buf, cvPoint((displayHeight+displayWidth-textSize.width)/2.0, y+textSize.height/2.0), &titleFonts[i], color);
}


void drawHexCorners(IplImage *out) {
    for (int ang = 0; ang < 360; ang += 60) {
        float rad = ang * M_PI / 180;
        CvPoint corner = cvPoint(2047*cos(rad),2047*sin(rad));
        CvPoint2D32f display_corner = project(displayMatrix, cvPoint2D32f(corner.x,corner.y));
        cvCircle(out, cvPoint(display_corner.x, display_corner.y), 10, CV_RGB(0,0,255), 5, CV_AA,0);
    }
}

void updateHUD(IplImage *out) {
    static double last_frame = 0.0;
    static float last_fps = 0.0;
    double now = timeNow();
    float fps = 1.0/(now-last_frame);
    last_frame = now;
    fps = (10*last_fps + fps) / 11.;
    last_fps = fps;

    if (!warpDisplay) {
        if (nextMousePoint!=4)
            cvPrintf(out, &hudFont, cvPoint(2, 20), CV_RGB(255,0,0), "%s: Click the %s corner", mouseOperationLabel[mouseOperation], mouseCornerLabel[nextMousePoint]);
        if (projection && !warpDisplay) {
            CvPoint corners[4], *rect = corners;
            int cornerCount = 4;
            for (int i=0; i<4; i++)
                corners[i] = cvPoint(projectionPoints[i].x*8, projectionPoints[i].y*8);
            cvPolyLine(out, &rect, &cornerCount, 1, 1, CV_RGB(0,255,255), 2, CV_AA, 3);
        }

        CvPoint textPoint = cvPoint(5, out->height-20);
        CvScalar textColor = CV_RGB(0,255,255);
        if (matchState == MATCH_ENDED)
            cvPrintf(out, &hudFont, textPoint, textColor, "Match ended.  Press <r> to start a new match.  %.1f FPS", fps);
        else if (matchState == MATCH_RUNNING) {
            float s = MATCH_LEN_SECONDS - (now - matchStartTime);
            cvPrintf(out, &hudFont, textPoint, textColor, "Remaining time: %02d:%6.3f seconds.  %.1f FPS", ((int)s)/60, fmod(s,60), fps);
        }
    } else {
        char buf[256];
        sprintf(buf, "Remaining Time:", fps);
        centeredFitTitleText(out, CV_RGB(255,255,255), 640, 200, buf);
                    

      

            
        //drawHexCorners(out);


        float s = MATCH_LEN_SECONDS - (now - matchStartTime);
        if (s < 0)
            s = 0;
        if (s > MATCH_LEN_SECONDS-1) {
            int state = s - MATCH_LEN_SECONDS + 1;
            int red = (state >= 2);
            int yel = (state == 1);
            int grn = (state <= 0);
            cvCircle(out, cvPoint((768+128)*8, 224*8), 65*8, red ? CV_RGB(255,40,40) : CV_RGB(64,0,0), -1, CV_AA, 3);
            cvCircle(out, cvPoint((768+128)*8, 384*8), 65*8, yel ? CV_RGB(255,255,80) : CV_RGB(64,64,0), -1, CV_AA, 3);
            cvCircle(out, cvPoint((768+128)*8, 544*8), 65*8, grn ? CV_RGB(80,255,80) : CV_RGB(0,64,0), -1, CV_AA, 3);
        }
        if (s > MATCH_LEN_SECONDS)
            s = MATCH_LEN_SECONDS;

        if (s > 15)
            sprintf(buf, "%2d:%04.1fs", ((int)s)/60, fmod(s,60));
        else
            sprintf(buf, "%5.2fs", fmod(s,60));

        CvScalar color;
        if (s < 15 && s != 0)
            color = CV_RGB(255,0,0);
        else
            color = CV_RGB(255,255,255);
        centeredFitTitleText(out, color, 670, 200, buf);

        if (showFPS) {
            sprintf(buf, "%4.1f FPS", fps);
            centeredFitTitleText(out, CV_RGB(255,255,255), 720, 200, buf);
        }

        int id_a = 0, id_b = 0;
        if (gameData.coords[0].id <= MAX_ROBOT_ID) {
            if (gameData.coords[0].y > 0)
                id_a = gameData.coords[0].id;
            else
                id_b = gameData.coords[0].id;
        }
        if (gameData.coords[1].id <= MAX_ROBOT_ID) {
            if (gameData.coords[1].y > 0)
                id_a = gameData.coords[1].id;
            else
                id_b = gameData.coords[1].id;
        }
        if (id_a <= MAX_ROBOT_ID)
            centeredFitTitleText(out, CV_RGB(255,255,255), 490, 200, teams[id_a]);
        if (id_b <= MAX_ROBOT_ID)
            centeredFitTitleText(out, CV_RGB(255,255,255), 585, 200, teams[id_b]);
    }
}

// detects robots and draws the HUD
void processSquares( IplImage *img, IplImage *out, IplImage *grayscale, CvSeq *squares ) {
    CvSeqReader reader;

    CvPoint pt[4];
    CvPoint2D32f bit_pt_true[16], trueCenter, orientationHandle;
    int id;
    float theta;

    // initialize reader of the sequence
    cvStartReadSeq( squares, &reader, 0 );
    // read 4 sequence elements at a time (all vertices of a square)
    for(int i=0; i<squares->total; i+=4) {
        for (int j=0; j<4; j++)
            CV_READ_SEQ_ELEM( pt[j], reader );

        if (!readPattern(img, pt, bit_pt_true, &id)) continue;

        // Kind of a hack - white squares get detected as Team 29, so
        // let's just disallow Team 29 from being recognized
        if (id == 29) continue;

        getCenterFromBits(bit_pt_true, &trueCenter);

        if (0)
            theta = getThetaFromAffine(bit_pt_true);
        else
            theta = getThetaFromExtension(bit_pt_true, trueCenter);
        processRobotDetection(trueCenter, theta, id, &orientationHandle);

        drawSquare(out, grayscale, pt, bit_pt_true, id, orientationHandle, theta);
    }
}

void sendStartStopCommand(int command, int id_a, int id_b) {
    packet_buffer packet;
    bzero(&packet, sizeof(packet));
    packet.type = command;
    packet.payload.array[0] = id_a;
    packet.payload.array[1] = id_b;

    serial_send_packet(fd_tx, &packet);
    printf("%s: %i, %i\n", command == START ? "start" : "stop", id_a, id_b);
}

void sendPositions(game_data gdata) {
    packet_buffer pos;
    bzero(&pos, sizeof(pos));
    
    pos.type = POSITION;
    pos.board = thisBoard;
    pos.seq_no = 0;
    memcpy(&pos.payload, &gdata, sizeof(game_data));
    serial_send_packet(fd_tx, &pos);
}

void *runSerial(void *params){
    while(1) {
        int sendPos, sendStart, sendStop;
        game_data localGameData;

        pthread_mutex_lock( &serial_lock );
        pthread_cond_wait( &serial_condition, &serial_lock );
        sendPos = sendPositionPacket;
        sendPositionPacket = 0;
        sendStart = sendStartPacket;
        if (sendStartPacket)
            sendStartPacket--;
        sendStop = sendStopPacket;
        if (sendStopPacket)
            sendStopPacket--;
        if (sendPos)
            memcpy(&localGameData, &serialGameData, sizeof(game_data));
        pthread_mutex_unlock(&serial_lock);

        if (sendPos)
            sendPositions(localGameData);

        if (sendStart)
            sendStartStopCommand(START, localGameData.coords[0].id, localGameData.coords[1].id);

        if (sendStop)
            sendStartStopCommand(STOP, localGameData.coords[0].id, localGameData.coords[1].id);
    }
}

void *runPlayingFieldSerial(void *params) {
    // Get a FILE* from the fd (TODO: switch to use fopen in serial.c?)
    FILE * file = fdopen(fd_pf, "r");
    while (1) {
        char strbuf[100];
        if ( fgets (strbuf , 100 , file) != NULL ) {
            //printf("Got %d chars\n", strlen(strbuf));
            //puts(strbuf);
            //continue;
            int teamA;
            int teamB;
            int scoreA;
            int scoreB;
            int rings_remaining[8];
            int rate_limit[8];
            /*
            strbuf[0] = 'D';
            strbuf[1] = 'A';
            strbuf[2] = 'T';
            strbuf[3] = 'A';
            strbuf[4] = ':';
            strbuf[5] = '5';
            strbuf[6] = '5';
            */
            int val = sscanf(strbuf, 
                             "DATA:%u,%u;%u,%u;%u,%u,%u,%u,%u,%u,%u,%u,;%u,%u,%u,%u,%u,%u,%u,%u;",
                             &teamA,
                             &teamB,
                             &scoreA,
                             &scoreB,
                             &rings_remaining[0],
                             &rings_remaining[1],
                             &rings_remaining[2],
                             &rings_remaining[3],
                             &rings_remaining[4],
                             &rings_remaining[5],
							 &rings_remaining[6],
							 &rings_remaining[7],
                             &rate_limit[0],
                             &rate_limit[1],
                             &rate_limit[2],
                             &rate_limit[3],
                             &rate_limit[4],
                             &rate_limit[5],
						 	 &rate_limit[6],
						 	 &rate_limit[7]
                             );
                printf("GOT: %s", strbuf);
            if (val > 0) {
                printf("Teams: %d, %d\n", teamA, teamB);
                if (teamA <= MAX_ROBOT_ID) {
                    scores[teamA] = scoreA;
                }
                if (teamB <= MAX_ROBOT_ID) {
                    scores[teamB] = scoreB;
                }
                
                for (int i = 0; i < 8; i++) {
                    gameData.dispenser[i].remaining = rings_remaining[i];
                    gameData.dispenser[i].rate_limit = rate_limit[i];
                }
            } else {
            }

        }
    }
}

int initSerial(const char *device, const char *playing_field_device) {
    if ((fd_tx = serial_open(device)) < 0)
        fprintf(stderr, "Could not open TX serial port!\n");
    serial_sync(fd_tx);

    //start the serial comm thread
    pthread_t serialThread;
    pthread_create( &serialThread, NULL, &runSerial, NULL);


    if ((fd_pf = serial_open(playing_field_device)) < 0) {
        fprintf(stderr, "Could not open playing field serial port!\n");
    } else {
        //start the serial comm thread
        pthread_t serialPlayingFieldThread;
        pthread_create( &serialPlayingFieldThread, NULL, &runPlayingFieldSerial, NULL);
    }

    return 0;
}

void cleanupSerial() {
    serial_close(fd_tx);
    serial_close(fd_pf);
}

void preserveValues(int id) {
    int params[] = {
        ball_threshold,
        canny_threshold,
        hough_votes,
        max_area,
        max_ball_dim,
        min_area,
        min_ball_dim,
        gameData.coords[0].id,
        gameData.coords[1].id,
        randomGoalSeed,
        side_tolerance,
        threshold
    };
    CvMat matrix = cvMat(sizeof(params)/sizeof(int),1,CV_32SC1,params);
    char buf[256];
    sprintf(buf, "Params%c.xml", boardLetter);
    cvSave( buf, &matrix, 0, 0, cvAttrList(0, 0) );
}

int initUI() {
    cvInitFont(&font, CV_FONT_HERSHEY_SIMPLEX, 1.0, 1.0, 0, 2, CV_AA);
    cvInitFont(&hudFont, CV_FONT_HERSHEY_SIMPLEX, 0.5, 0.5, 0, 0, CV_AA);
    float w;
    w=1.2;cvInitFont(&titleFonts[0], CV_FONT_HERSHEY_DUPLEX, w, w, 0, 1, CV_AA);
    w=1.0;cvInitFont(&titleFonts[1], CV_FONT_HERSHEY_DUPLEX, w, w, 0, 1, CV_AA);
    w=.8;cvInitFont(&titleFonts[2], CV_FONT_HERSHEY_DUPLEX, w, w, 0, 1, CV_AA);
    w=.6;cvInitFont(&titleFonts[3], CV_FONT_HERSHEY_DUPLEX, w, w, 0, 1, CV_AA);

    //cvStartWindowThread();

    cvNamedWindow( WND_MAIN, 1 );
    cvResizeWindow( WND_MAIN, displayWidth, displayHeight);
    cvNamedWindow( WND_CONTROLS, 1);
    cvResizeWindow( WND_CONTROLS, 200, 400);
#if SHOW_FILTERED_OUTPUT
    cvNamedWindow( WND_FILTERED, CV_WINDOW_AUTOSIZE);
#endif
    cvCreateTrackbar( TRK_THRESHOLD, WND_CONTROLS, &threshold, 255, &preserveValues);
    cvCreateTrackbar( TRK_TOLERANCE, WND_CONTROLS, &side_tolerance, 300, &preserveValues);
    cvCreateTrackbar( TRK_MIN_AREA, WND_CONTROLS, &min_area, 10000, &preserveValues);
    cvCreateTrackbar( TRK_MAX_AREA, WND_CONTROLS, &max_area, 10000, &preserveValues);
    cvCreateTrackbar( TRK_CANNY_THRESHOLD, WND_CONTROLS, &canny_threshold, 255, &preserveValues);
    cvCreateTrackbar( TRK_HOUGH_VOTES, WND_CONTROLS, &hough_votes, 100, &preserveValues);
    cvCreateTrackbar( TRK_MIN_BALL_DIM, WND_CONTROLS, &min_ball_dim, 50, &preserveValues);
    cvCreateTrackbar( TRK_MAX_BALL_DIM, WND_CONTROLS, &max_ball_dim, 50, &preserveValues);
    cvCreateTrackbar( TRK_BALL_THRESHOLD, WND_CONTROLS, &ball_threshold, 255, &preserveValues);

    cvSetMouseCallback(WND_MAIN,mouseHandler, NULL);

    return 0;
}

void cleanupUI() {
    cvDestroyWindow( WND_MAIN);
}

int initCV(char *source) {
    // create memory storage for contours
    storage = cvCreateMemStorage(0);

    capture = 0;

    int i = 0;
    if (source && sscanf(source, "%d", &i) != 1)
        i = -1;

    if (i!=-1)
        capture = cvCaptureFromCAM(i);
    else if (source)
        capture = cvCaptureFromAVI( source );

    if( !capture ) {
        fprintf(stderr,"Could not initialize capturing...\n");
        return -1;
    }

    /*
    //setup camera properties
    cvSetCaptureProperty(capture, CV_CAP_PROP_BRIGHTNESS, 0.75);
    cvSetCaptureProperty(capture, CV_CAP_PROP_CONTRAST, 100);
    cvSetCaptureProperty(capture, CV_CAP_PROP_SATURATION, 0);
    cvSetCaptureProperty(capture, CV_CAP_PROP_BRIGHTNESS, 0.25);
    cvSetCaptureProperty(capture, CV_CAP_PROP_EXPOSURE, 0.2);
    cvSetCaptureProperty(capture, CV_CAP_PROP_GAIN, 0);

    //printf("PROPERTY: %f\n",cvGetCaptureProperty( capture, CV_CAP_PROP_MODE ));
    */
    frameWidth = 640;
    frameHeight = 480;
    cvSetCaptureProperty( capture, CV_CAP_PROP_FRAME_WIDTH, frameWidth);
    cvSetCaptureProperty( capture, CV_CAP_PROP_FRAME_HEIGHT, frameHeight);
    printf("%f, %f\n", frameWidth, frameHeight);
    min_area *= (frameWidth*frameWidth)/(1000*1000);
    max_area *= (frameWidth*frameWidth)/(1000*1000);

    return 0;
}

void cleanupCV() {
    if (projection)
        cvReleaseMat(&projection);
    if (invProjection)
        cvReleaseMat(&invProjection);
}

int initGame() {
    return 0;
}

CvMat *coviM = 0, *muM = 0;
int hasStarted = 0;
int handleKeypresses() {
    char c = cvWaitKey(5); // cvWaitKey takes care of event processing
    if( c == 27 )  //ESC
        return -1;
    else if ( c == 'i' ) {
        mouseOperation = PICK_PROJECTION_CORNERS;
        nextMousePoint = 0;
    } else if ( c == 'r' ) {
        hasStarted = 0;
        matchStartTime = timeNow()+2.0; //set the match start time
        matchState = MATCH_RUNNING;
        for (int i = 0; i < 8; i++) {
            gameData.dispenser[i].remaining = 16;
            gameData.dispenser[i].rate_limit = 0;
        }
        resetRound(randomGoalSeed);
    } else if ( c == 'R' ) {
        matchStartTime = timeNow()-MATCH_LEN_SECONDS;
        pthread_mutex_lock(&serial_lock);
        sendStartPacket = 0;
        sendStopPacket = 0;
        matchState = MATCH_ENDED;
        pthread_mutex_unlock(&serial_lock);
    } else if ( c == '!' ) {
        matchStartTime = timeNow()-MATCH_LEN_SECONDS;
        pthread_mutex_lock(&serial_lock);
        sendStartPacket = 0;
        sendStopPacket = 10;
        matchState = MATCH_ENDED;
        pthread_mutex_unlock(&serial_lock);
    } else if ( c == 's' ) {
        mouseOperation = PICK_SAMPLE_CORNERS;
        nextMousePoint = 0;
    } else if ( c == '+' ) {
        if (coviM) {
            cvScale(coviM, coviM, sqrt(0.5), 0);
            cvScale(muM, muM, sqrt(0.5), 0);
        }
    } else if ( c == '-' ) {
        if (coviM) {
            cvScale(coviM, coviM, sqrt(2.0), 0);
            cvScale(muM, muM, sqrt(2.0), 0);
        }
    } else if ( c == 'p' ){
        warpDisplay = !warpDisplay;
        computeDisplayMatrix();
    } else if ( c == 'e' ){
        // exclude
        mouseOperation = PICK_EXCLUDE_CORNERS;
        nextMousePoint = 0;
    } else if ( c == 'E' ){
        nextExclude = 0;
        for (int i=0; i<10; i++)
            for (int j=0; j<4; j++)
                excludeCorners[i][j] = cvPoint2D32f(0,0);
        saveExclusions();
    } else if ( c == 'f' ) {
        showFPS = !showFPS;
    } else if ( c == 'z' ) {
        showPhotoFinish = !showPhotoFinish;
    } else if ( c == ' ' ) {
        showFilteredSquares = !showFilteredSquares;
        if (showFilteredSquares) {
            cvNamedWindow( WND_FILTERED_SQUARES, 0);
            cvResizeWindow( WND_FILTERED_SQUARES, frameWidth, frameHeight);
        } else {
            cvDestroyWindow( WND_FILTERED_SQUARES);
        }
    }
    return 0;
}

void updateGame() {
    double now = timeNow();
    if (matchState == MATCH_RUNNING && (now - matchStartTime) >= 0 && !hasStarted) {
        pthread_mutex_lock(&serial_lock);
        sendStartPacket = 10; //set flag for start packet to be sent
        pthread_mutex_unlock(&serial_lock);
        hasStarted = 1;
    }
    if ((now - matchStartTime) >= MATCH_LEN_SECONDS && matchState != MATCH_ENDED) {
        matchState = MATCH_ENDED;
        pthread_mutex_lock(&serial_lock);
        sendStopPacket = 10;
        pthread_mutex_unlock(&serial_lock);
    }
}

// compute hermitian square root H of inverse of symmetric matrix M with real positive eigenvalues
// so that xT HT H x = xT M x
CvMat *symmInvSqrt(CvMat *M) {
    CvMat *W, *U, *V, *T;
    W = cvCreateMat(M->rows, M->cols, CV_32FC1);
    U = cvCreateMat(M->rows, M->cols, CV_32FC1);
    V = cvCreateMat(M->rows, M->cols, CV_32FC1);
    T = cvCreateMat(M->rows, M->cols, CV_32FC1);
    cvSVD(M, W, U, V, CV_SVD_U_T|CV_SVD_V_T); // M = U W V^T
    cvTranspose(U, U);

    for (int i=0; i<M->rows; i++)
        cvmSet(W, i, i, cvInvSqrt(cvmGet(W, i, i)));

    cvMatMul(U, W, T);
    cvMatMul(T, V, W);

    cvReleaseMat(&U);
    cvReleaseMat(&V);
    cvReleaseMat(&T);
    return W;
}

void sampleColorModel(IplImage *img) {
    IplImage *work = cvCreateImage(cvSize(img->width, img->height), 8, 3);
    //cvCvtColor(img, work, CV_BGR2Lab);
    cvSmooth(img, work, CV_GAUSSIAN, 7, 7, 0, 0);
    IplImage *mask = cvCreateImage(cvSize(img->width, img->height), 8, 1);
    CvPoint pts[4], *quad = pts;
    int count = 4;
    cvSet(mask, cvRealScalar(0), 0);
    for (int i=0; i<4; i++)
    pts[i] = cvPoint(sampleCorners[i].x, sampleCorners[i].y);
    cvFillPoly(mask, &quad, &count, 1, cvRealScalar(255), 8, 0);

    float sxy[3][3], sx[3], cov[3][3], mu[3];
    int n=0;
    for (int i=0;i<3;i++) {
        sx[i] = 0;
        for (int j=0;j<3;j++) {
            sxy[i][j] = 0;
            cov[i][j] = 0;
        }
    }
    for (int y=0; y<img->height; y++) {
        for (int x=0; x<img->width; x++) {
            if (cvGet2D(mask, y, x).val[0]) {
                CvScalar rgb = cvGet2D(work, y, x);
                for (int i=0;i<3;i++) {
                    sx[i] += rgb.val[i];
                    for (int j=0;j<3;j++)
                        sxy[i][j] += rgb.val[i] * rgb.val[j];
                }
                n++;
            }
        }
    }
    for (int i=0;i<3;i++) {
        for (int j=0;j<3;j++)
            cov[i][j] = (sxy[i][j]/n - sx[i]*sx[j]/n/n);
        mu[i] = sx[i]/n;
        printf("%d %6.2f  %6.2f %6.2f %6.2f\n", i, mu[i], cov[i][0], cov[i][1], cov[i][2]);
        mu[i] *= 128;
    }

    sampleColors = 0;

    cvReleaseImage(&work);
    cvReleaseImage(&mask);

    CvMat covM = cvMat(3,3,CV_32FC1,cov);
    coviM = symmInvSqrt(&covM);
    CvMat *T = cvCreateMat(3,3,CV_32FC1);
    cvMatMul(&covM, coviM, T);
    cvMatMul(T, coviM, &covM);
    // coviM has magnitude ~ 1/256.
    cvScale(coviM, coviM, sqrt(0x1000)/128., 0.);
    // coviM^2 has magnitude ~ 1/262144.
    cvReleaseMat(&T);
    muM = cvCreateMat(3,1,CV_32FC1);
    for (int i=0;i<3;i++)
    cvmSet(muM, i, 0, -mu[i]);
    cvMatMul(coviM, muM, muM);
}

int main(int argc, char** argv) {
    if (argc != 5){
        fprintf(stderr,"Need 3 arguments: [camera number] [serial port] [table letter]\n");
        exit(-1);
    }

    thisBoard = atoi(argv[1]); //use camera id as board number
    boardLetter = argv[3][0]; //use camera id as board number
    printf("This board: %i (%c)\n", thisBoard, boardLetter);
    printf("To initialize coordinate projection, press <i>\n");

    projectionPoints[0] = cvPoint2D32f(0, 0);
    projectionPoints[1] = cvPoint2D32f(frameWidth, 0);
    projectionPoints[2] = cvPoint2D32f(frameWidth, frameHeight);
    projectionPoints[3] = cvPoint2D32f(0, frameHeight);

    char buf[256];
    sprintf(buf, "Exclusions%c.xml", boardLetter);
	CvMat *excludePts = (CvMat*)cvLoad( buf, 0, 0, 0);
    sprintf(buf, "Projection%c.xml", boardLetter);
	CvMat *projPts = (CvMat*)cvLoad( buf, 0, 0, 0);
    sprintf(buf, "Params%c.xml", boardLetter);
	CvMat *params = (CvMat*)cvLoad( buf, 0, 0, 0);
    if (projPts) {
        for (int i=0; i<4; i++)
            projectionPoints[i] = CV_MAT_ELEM(*projPts, CvPoint2D32f, i, 0);
    }
    if (excludePts) {
        for (int i=0; i<10; i++) {
            int allZeros = 1;
            for (int j=0; j<4; j++) {
                excludeCorners[i][j] = CV_MAT_ELEM(*excludePts, CvPoint2D32f, i, j);
                if (excludeCorners[i][j].x != 0 || excludeCorners[i][j].y != 0)
                    allZeros = 0;
            }
            if (allZeros) {
                nextExclude = i;
                break;
            }
        }
    } else {
        nextExclude = 0;
        for (int i=0; i<10; i++)
            for (int j=0; j<4; j++)
                excludeCorners[i][j] = cvPoint2D32f(0,0);
    }
    if (params) {
        ball_threshold = CV_MAT_ELEM(*params, int, 0, 0);
        canny_threshold = CV_MAT_ELEM(*params, int, 1, 0);
        hough_votes = CV_MAT_ELEM(*params, int, 2, 0);
        max_area = CV_MAT_ELEM(*params, int, 3, 0);
        max_ball_dim = CV_MAT_ELEM(*params, int, 4, 0);
        min_area = CV_MAT_ELEM(*params, int, 5, 0);
        min_ball_dim = CV_MAT_ELEM(*params, int, 6, 0);
        randomGoalSeed = CV_MAT_ELEM(*params, int, 9, 0);
        side_tolerance = CV_MAT_ELEM(*params, int, 10, 0);
        threshold = CV_MAT_ELEM(*params, int, 11, 0);
    }
    cvReleaseMat(&projPts);
    cvReleaseMat(&params);

    if (initSerial(argc>2 ? argv[2] : NULL, argc>4 ? argv[4] : NULL)) return -1;
    if (initUI()) return -1;
    if (initCV(argc>1 ? argv[1] : NULL)) return -1;
    if (initGame()) return -1;

    IplImage *mask8 = 0, *mask = 0, *dev = 0, *grayscale = 0, *out = 0, *sidebar = 0;
    IplImage *photoFinish = 0;
    {
        sprintf(buf, "sidebar%c.png", boardLetter);
        IplImage *sidebar2 = cvLoadImage(buf, CV_LOAD_IMAGE_COLOR);
        sidebar = cvCreateImage(cvSize(sidebar2->width, sidebar2->height), 8, 3);
        cvConvertScale(sidebar2, sidebar, 1, 0);
        cvReleaseImage(&sidebar2);
    }

    warpDisplay = 1;
    projection_init(&projection, &invProjection, projectionPoints, bounds);
    computeDisplayMatrix();

    for (int i=0; i<MAX_ROBOT_ID+1; i++)
        sightings[i] = 0;

    for (int i=0; i<MAX_ROBOT_ID; i++) {
        teams[i] = (char *)malloc(256);
        sprintf(teams[i], "");
    }
    FILE *f = fopen("teams.tsv", "r");
    while (!feof(f)) {
        int i;
        fscanf(f, "%d ", &i);
        fgets(buf, 256, f);
        if (buf[strlen(buf)-1] == '\n')
            buf[strlen(buf)-1] = '\0';
        strncpy(teams[i], buf, 256);
    }
    fclose(f);

    double lastPosUpdate = timeNow();
    double lastStartPacket = timeNow();
    while(1) {
        IplImage *frame = cvQueryFrame( capture );
        if( !frame ) {
            fprintf(stderr,"cvQueryFrame failed!\n");
            continue;
        }

        IplImage *img = cvCloneImage(frame); // can't modify original

        if (!out)
            out = cvCreateImage(cvSize(img->width, img->height), 8, 3);

        if (warpDisplay) {
            if (out->width != displayWidth) {
                cvReleaseImage(&out);
                out = cvCreateImage(cvSize(displayWidth, displayHeight), 8, 3);
            }
            cvSetImageROI(out, cvRect(out->height, 0, out->width-out->height, out->height));
            cvSetImageROI(sidebar, cvRect(0, 0, out->width-out->height, out->height));
            cvCopy(sidebar, out, 0);
            cvResetImageROI(out);
            cvSetImageROI(out, cvRect(0, 0, out->height, out->height));
        } else {
            if (out->width != img->width) {
                cvReleaseImage(&out);
                out = cvCreateImage(cvSize(img->width, img->height), 8, 3);
            }
            cvResetImageROI(out);
        }

        CvMat *M = cvCreateMat(3,3, CV_32FC1);
        cvMatMul(displayMatrix, projection, M);
        cvWarpPerspective(img, out, M, CV_INTER_LINEAR + CV_WARP_FILL_OUTLIERS, CV_RGB(0,0,0));
        cvReleaseMat(&M);

        //if (warpDisplay) {
            //cvRectangle(out, cvPoint(frameHeight,0), cvPoint(frameWidth,frameHeight), CV_RGB(0,0,0), CV_FILLED, 0, 0);
        //}

        if (!grayscale)
            grayscale = cvCreateImage(cvSize(img->width, img->height), 8, 1);
        cvCvtColor(img, grayscale, CV_BGR2GRAY);
        CvSeq *squares = findCandidateSquares(grayscale);
        processSquares(img, out, grayscale, squares);

        robots[0].id = 0xAA;
        int id_a=0, id_b=0, votes_a=0, votes_b=0;
        for (int i=0; i<MAX_ROBOT_ID+1; i++) {
            sightings[i] = MIN(MAX(sightings[i]-1, 0), 60);
            if (sightings[i] > votes_a) {
                votes_b = votes_a;
                id_b = id_a;
                votes_a = sightings[i];
                id_a = i;
            } else if (sightings[i] > votes_b) {
                votes_b = sightings[i];
                id_b = i;
            }
        }
        gameData.coords[0] = robots[id_a];
        gameData.coords[1] = robots[id_b];

        for (int i=0; i<nextExclude; i++) {
            CvPoint pt[4], *p = pt;
            int count = 4;
            for (int j=0; j<4; j++)
                pt[j] = cvPoint(excludeCorners[i][j].x,excludeCorners[i][j].y);
            cvFillPoly(grayscale, &p, &count, 1, CV_RGB(0,0,0), 8, 0);
            if (!warpDisplay)
                cvPolyLine(out, &p, &count, 1, 1, CV_RGB(255,0,0), 2, CV_AA, 0);
        }

        cvResetImageROI( out );

        updateHUD(out);

        if (handleKeypresses())
            break;
        int stateBefore = matchState;
        updateGame();
        if (stateBefore == MATCH_RUNNING && matchState == MATCH_ENDED) {
            if (photoFinish)
                cvReleaseImage(&photoFinish);
            photoFinish = cvCloneImage(out);
            showPhotoFinish = 1;
        }

        double now = timeNow();
        if (now - lastStartPacket > 5.0 && (now - matchStartTime > 3) && matchState == MATCH_RUNNING) {
            pthread_mutex_lock( &serial_lock);
            sendStartPacket++;
            pthread_mutex_unlock( &serial_lock);

            lastStartPacket = now;
        }
        if (now - lastPosUpdate > 1.0 || matchState == MATCH_RUNNING || 1) {
            pthread_mutex_lock( &serial_lock);
            memcpy(&serialGameData, &gameData, sizeof(game_data));
            sendPositionPacket = 1;
            pthread_mutex_unlock( &serial_lock);

            lastPosUpdate = now;
        }

        pthread_mutex_lock( &serial_lock);
        if (sendPositionPacket || sendStartPacket || sendStopPacket)
            pthread_cond_signal( &serial_condition);
        pthread_mutex_unlock( &serial_lock);

        // show the resultant image
        cvShowImage( WND_MAIN, (showPhotoFinish && photoFinish) ? photoFinish : out );

        cvReleaseImage(&img);
        cvClearMemStorage(storage); // clear memory storage - reset free space position
    }

    if (out)
        cvReleaseImage( &out );

    if (mask8) {
        cvReleaseImage(&dev);
        cvReleaseImage(&mask);
        cvReleaseImage(&mask8);
    }
    if (grayscale)
        cvReleaseImage(&grayscale);

    if (coviM)
        cvReleaseMat(&coviM);
    if (muM)
        cvReleaseMat(&muM);

    cleanupCV();
    cleanupUI();
    cleanupSerial();

    return 0;
}
