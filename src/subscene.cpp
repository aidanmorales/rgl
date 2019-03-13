#include "subscene.h"
#include "rglview.h"
#include "select.h"
#include "gl2ps.h"
#include "R.h"
#include <algorithm>
#include <functional>

using namespace rgl;

//////////////////////////////////////////////////////////////////////////////
//
// CLASS
//   Subscene
//

Subscene::Subscene(Embedding in_viewport, Embedding in_projection, Embedding in_model,
                   Embedding in_mouseHandlers,
                   bool in_ignoreExtent)
 : SceneNode(SUBSCENE), parent(NULL), do_viewport(in_viewport), do_projection(in_projection),
   do_model(in_model), do_mouseHandlers(in_mouseHandlers), 
   viewport(0.,0.,1.,1.),Zrow(), Wrow(),
   pviewport(0,0,1024,1024), drag(0), ignoreExtent(in_ignoreExtent),
   selectState(msNONE), 
   dragBase(0.0f,0.0f), dragCurrent(0.0f,0.0f)
{
  userviewpoint = NULL;
  modelviewpoint = NULL;
  bboxdeco   = NULL;
  background = NULL;
  bboxChanges = false;
  data_bbox.invalidate();
  modelMatrix.setIdentity();
  projMatrix.setIdentity(); 
  mouseListeners.push_back(this);
  for (int i=0; i<3; i++) {
    beginCallback[i] = NULL;
    updateCallback[i] = NULL;
    endCallback[i] = NULL;
    cleanupCallback[i] = NULL;
    for (int j=0; j<3; j++) 
      userData[3*i + j] = NULL;
  }
  setDefaultMouseMode();
}

Subscene::~Subscene() 
{
  for (std::vector<Subscene*>::iterator i = subscenes.begin(); i != subscenes.end(); ++ i ) 
    delete (*i);
  for (int i=0; i<3; i++) 
    if (cleanupCallback[i]) 
      (*cleanupCallback[i])(userData + 3*i);
}

bool Subscene::add(SceneNode* node)
{
  bool success = false;
  switch( node->getTypeID() )
  {
    case SHAPE:
      {
        Shape* shape = (Shape*) node;
        addShape(shape);

        success = true;
      }
      break;
    case LIGHT:
      {
	Light* light = (Light*) node;
	  addLight(light);
	  
	  success = true;
      }
      break;
    case USERVIEWPOINT:
      {
        userviewpoint = (UserViewpoint*) node;
        success = true;
      }
      break;
    case MODELVIEWPOINT:
      {
        modelviewpoint = (ModelViewpoint*) node;
        success = true;
      }
      break;
    case SUBSCENE:
      {
	Subscene* subscene = static_cast<Subscene*>(node);
	if (subscene->parent)
	  error("Subscene %d is already a child of subscene %d.", subscene->getObjID(),
	        subscene->parent->getObjID());
	addSubscene(subscene);
	success = true;
      }
      break;
    case BACKGROUND:
      {
        Background* background = static_cast<Background*>(node);
        addBackground(background);
        success = true;
      }
      break;
    case BBOXDECO:
      { 
        BBoxDeco* bboxdeco = static_cast<BBoxDeco*>(node);
        addBBoxDeco(bboxdeco);
        success = true;
      }
      break;
    default:
      break;
  }
  return success;
}

void Subscene::addBackground(Background* newbackground)
{
  background = newbackground;
}

void Subscene::addBBoxDeco(BBoxDeco* newbboxdeco)
{
  bboxdeco = newbboxdeco;
}

void Subscene::addShape(Shape* shape)
{
  if (!shape->getIgnoreExtent()) 
    addBBox(shape->getBoundingBox(), shape->getBBoxChanges());

  shapes.push_back(shape);
  
  if ( shape->isBlended() ) {
    zsortShapes.push_back(shape);
  } else if ( shape->isClipPlane() ) {
    clipPlanes.push_back(static_cast<ClipPlaneSet*>(shape));
    shrinkBBox();
  } else
    unsortedShapes.push_back(shape);
}

void Subscene::addBBox(const AABox& bbox, bool changes)
{
  data_bbox += bbox;
  bboxChanges |= changes;
  intersectClipplanes();
  if (parent && !ignoreExtent) 
    parent->addBBox(data_bbox, changes);
}
  
void Subscene::addLight(Light* light)
{
  lights.push_back(light);
}

void Subscene::addSubscene(Subscene* subscene)
{
  subscenes.push_back(subscene);
  subscene->parent = this;
  subscene->newEmbedding();
  if (!subscene->getIgnoreExtent()) 
    addBBox(subscene->getBoundingBox(), subscene->bboxChanges);
}

void Subscene::hideShape(int id)
{
  std::vector<Shape*>::iterator ishape 
     = std::find_if(shapes.begin(), shapes.end(), 
       std::bind2nd(std::ptr_fun(&sameID), id));
  if (ishape == shapes.end()) return;
        
  Shape* shape = *ishape;
  shapes.erase(ishape);
  if ( shape->isBlended() )
    zsortShapes.erase(std::find_if(zsortShapes.begin(), zsortShapes.end(),
                                   std::bind2nd(std::ptr_fun(&sameID), id)));
  else if ( shape->isClipPlane() )
    clipPlanes.erase(std::find_if(clipPlanes.begin(), clipPlanes.end(),
                     std::bind2nd(std::ptr_fun(&sameID), id)));
  else
    unsortedShapes.erase(std::find_if(unsortedShapes.begin(), unsortedShapes.end(),
                         std::bind2nd(std::ptr_fun(&sameID), id)));
      
  shrinkBBox();
}

void Subscene::hideLight(int id)
{
  std::vector<Light*>::iterator ilight = std::find_if(lights.begin(), lights.end(),
                            std::bind2nd(std::ptr_fun(&sameID), id));
  if (ilight != lights.end()) {
    lights.erase(ilight);
  }
}

void Subscene::hideBBoxDeco(int id)
{
  if (bboxdeco && sameID(bboxdeco, id))
    bboxdeco = NULL;
}

void Subscene::hideBackground(int id)
{
  if (background && sameID(background, id)) {
    if (parent)
      background = NULL;
    else
      background = new( Background );  /* The root must always have a background */
  }
}

Subscene* Subscene::hideSubscene(int id, Subscene* current)
{
  for (std::vector<Subscene*>::iterator i = subscenes.begin(); i != subscenes.end(); ++ i) {
    if (sameID(*i, id)) {
      if ((*i)->getSubscene(current->getObjID()))
        current = (*i)->parent;  
      (*i)->parent = NULL;
      subscenes.erase(i);
      shrinkBBox();
      return current;
    } 
  }
  return current;
}

void Subscene::hideViewpoint(int id)
{
  if (userviewpoint && sameID(userviewpoint, id)) {
    if (parent)            /* the root needs a viewpoint */
      userviewpoint = NULL;
  } else if (modelviewpoint && sameID(modelviewpoint, id)) {
    if (parent)            /* the root needs a viewpoint */
      modelviewpoint = NULL;
  } 
}

Subscene* Subscene::getSubscene(int id)
{
  if (id == getObjID()) return this;
    
  for (std::vector<Subscene*>::iterator i = subscenes.begin(); i != subscenes.end() ; ++ i ) {
    Subscene* subscene = (*i)->getSubscene(id);
    if (subscene) return subscene;
  }
  
  return NULL;
}

Subscene* Subscene::whichSubscene(int id)
{
  for (std::vector<Shape*>::iterator i = shapes.begin(); i != shapes.end() ; ++ i ) {
    if ((*i)->getObjID() == id)
      return this;
  }
  for (std::vector<Light*>::iterator i = lights.begin(); i != lights.end() ; ++ i ) {
    if ((*i)->getObjID() == id)
      return this;
  }
  if (bboxdeco && bboxdeco->getObjID() == id)
    return this;
  for (std::vector<Subscene*>::iterator i = subscenes.begin(); i != subscenes.end(); ++ i ) {
    if ((*i)->getObjID() == id)
      return this;
  }
  if (userviewpoint && userviewpoint->getObjID() == id)
    return this;
  if (modelviewpoint && modelviewpoint->getObjID() == id)
    return this;
  if (background && background->getObjID() == id)
    return this;
  for (std::vector<Subscene*>::iterator i = subscenes.begin(); i != subscenes.end() ; ++ i ) {
    Subscene* result = (*i)->whichSubscene(id);
    if (result) 
      return result;
  }
  return NULL;
}

Subscene* Subscene::whichSubscene(int mouseX, int mouseY)
{
  Subscene* result = NULL;
  Subscene* sub;
  for (std::vector<Subscene*>::iterator i = subscenes.begin(); i != subscenes.end() ; ++ i ) {
    result = (sub = (*i)->whichSubscene(mouseX, mouseY)) ? sub : result;
  }  
  if (!result && pviewport.x <= mouseX && mouseX < pviewport.x + pviewport.width 
              && pviewport.y <= mouseY && mouseY < pviewport.y + pviewport.height)
    result = this;
  return result;
}

int Subscene::getAttributeCount(AABox& bbox, AttribID attrib)
{
  switch (attrib) {
    case IDS:	   
    case TYPES:    return shapes.size();
  }
  return SceneNode::getAttributeCount(bbox, attrib);
}

void Subscene::getAttribute(AABox& bbox, AttribID attrib, int first, int count, double* result)
{
  int n = getAttributeCount(bbox, attrib);
  int ind = 0;

  if (first + count < n) n = first + count;
  if (first < n) {
    switch(attrib) {
      case IDS:
        for (std::vector<Shape*>::iterator i = shapes.begin(); i != shapes.end() ; ++ i ) {
      	  if ( first <= ind  && ind < n )  
            *result++ = (*i)->getObjID();
          ind++;
        }
        return;
    }  
    SceneNode::getAttribute(bbox, attrib, first, count, result);
  }
}

String Subscene::getTextAttribute(AABox& bbox, AttribID attrib, int index)
{
  int n = getAttributeCount(bbox, attrib);
  if (index < n && attrib == TYPES) {
    char* buffer = R_alloc(20, 1);    
    shapes[index]->getTypeName(buffer, 20);
    return String(strlen(buffer), buffer);
  } else
    return SceneNode::getTextAttribute(bbox, attrib, index);
}

void Subscene::renderClipplanes(RenderContext* renderContext)
{
  std::vector<ClipPlaneSet*>::iterator iter;
  
  ClipPlaneSet::num_planes = 0;
	
  for (iter = clipPlanes.begin() ; iter != clipPlanes.end() ; ++iter ) {
    ClipPlaneSet* plane = *iter;
    plane->render(renderContext);
    SAVEGLERROR;
  }
}

void Subscene::disableClipplanes(RenderContext* renderContext)
{
  std::vector<ClipPlaneSet*>::iterator iter;
	
  for (iter = clipPlanes.begin() ; iter != clipPlanes.end() ; ++iter ) {
    ClipPlaneSet* plane = *iter;
    plane->enable(false);
    SAVEGLERROR;
  }
}
 
int Subscene::get_id_count(TypeID type, bool recursive)
{
  int result = 0;
  if (recursive)
    for (std::vector<Subscene*>::iterator i = subscenes.begin(); i != subscenes.end(); ++ i ) 
      result += (*i)->get_id_count(type, recursive);
  switch (type) {
    case SHAPE: {
      result += shapes.size();
      break;
    }
    case LIGHT: {
      result += lights.size();
      break;
    }
    case BBOXDECO: {
      result += bboxdeco ? 1 : 0;
      break;
    }
    case SUBSCENE: {
      result += subscenes.size();
      break;
    }
    case USERVIEWPOINT: {    
      result += do_projection > EMBED_INHERIT ? 1 : 0;
      break;
    }
    case MODELVIEWPOINT: {    
      result += do_model > EMBED_INHERIT ? 1 : 0;
      break;
    }
    case BACKGROUND: {
      result += background ? 1 : 0;
      break;
    }
  }
  return result;
}
    
int Subscene::get_ids(TypeID type, int* ids, char** types, bool recursive)
{
  char buffer[20];
  int count = 0;
  switch(type) {
  case SHAPE: 
    for (std::vector<Shape*>::iterator i = shapes.begin(); i != shapes.end() ; ++ i ) {
      *ids++ = (*i)->getObjID();
      buffer[19] = 0;
      (*i)->getTypeName(buffer, 20);
      *types = R_alloc(strlen(buffer)+1, 1);
      strcpy(*types, buffer);
      types++;
      count++;
    }
    break;
  case LIGHT: 
    for (std::vector<Light*>::iterator i = lights.begin(); i != lights.end() ; ++ i ) {
      *ids++ = (*i)->getObjID();
      *types = R_alloc(strlen("light")+1, 1);
      strcpy(*types, "light");
      types++;
      count++;
    }
    break;
  case BBOXDECO: 
    if (bboxdeco) {
      *ids++ = bboxdeco->getObjID();
      *types = R_alloc(strlen("bboxdeco")+1, 1);
      strcpy(*types, "bboxdeco");
      types++;
      count++;
    }
    break;
  case SUBSCENE: 
    for (std::vector<Subscene*>::iterator i = subscenes.begin(); i != subscenes.end(); ++ i ) {
      *ids++ = (*i)->getObjID();
      *types = R_alloc(strlen("subscene")+1, 1);
      strcpy(*types, "subscene");
      types++;
      count++;
    }
    break;
  case USERVIEWPOINT:
    if (userviewpoint) {
      *ids++ = userviewpoint->getObjID();
      *types = R_alloc(strlen("userviewpoint")+1, 1);
      strcpy(*types, "userviewpoint");
      types++;
      count++;
    }
    break;
  case MODELVIEWPOINT:
    if (modelviewpoint) {
      *ids++ = modelviewpoint->getObjID();
      *types = R_alloc(strlen("modelviewpoint")+1, 1);
      strcpy(*types, "modelviewpoint");
      types++;
      count++;
    }
    break;
  case BACKGROUND:
    if (background) {
      *ids++ = background->getObjID();
      *types = R_alloc(strlen("background")+1, 1);
      strcpy(*types, "background");
      types++;
      count++;
    }
    break;
  }
  if (recursive)
    for (std::vector<Subscene*>::iterator i = subscenes.begin(); i != subscenes.end(); ++ i ) {
      int newcount = (*i)->get_ids(type, ids, types, true);
      ids += newcount;
      types += newcount;
      count += newcount;
    }
  return count;
}

Background* Subscene::get_background()
{
  if (background) return background;
  else if (parent) return parent->get_background();
  else return NULL;
}

Background* Subscene::get_background(int id)
{
  Background* background = get_background();
  if (background && background->getObjID() == id)
    return background;
  
  std::vector<Subscene*>::const_iterator iter;
  for(iter = subscenes.begin(); iter != subscenes.end(); ++iter) {
    background = (*iter)->get_background(id);
    if (background) return background;
  }
  return NULL;
}  

BBoxDeco* Subscene::get_bboxdeco()
{
  if (bboxdeco) return bboxdeco;
  else if (parent) return parent->get_bboxdeco();
  else return NULL;
}

BBoxDeco* Subscene::get_bboxdeco(int id)
{
  BBoxDeco* bboxdeco = get_bboxdeco();
  if (bboxdeco && bboxdeco->getObjID() == id)
    return bboxdeco;
  
  std::vector<Subscene*>::const_iterator iter;
  for(iter = subscenes.begin(); iter != subscenes.end(); ++iter) {
    bboxdeco = (*iter)->get_bboxdeco(id);
    if (bboxdeco) return bboxdeco;
  }
  return NULL;
}  


UserViewpoint* Subscene::getUserViewpoint()
{
  if (userviewpoint && do_projection > EMBED_INHERIT)
    return userviewpoint;
  else if (parent) return parent->getUserViewpoint();
  else error("must have a user viewpoint");
}

ModelViewpoint* Subscene::getModelViewpoint()
{
  if (modelviewpoint && do_model > EMBED_INHERIT)
    return modelviewpoint;
  else if (parent) return parent->getModelViewpoint();
  else error("must have a model viewpoint");
}

void Subscene::update(RenderContext* renderContext)
{
  GLdouble saveprojection[16];
  

  renderContext->subscene = this;
  
  setupViewport(renderContext);
  if (bboxChanges) 
    calcDataBBox();
  
  Sphere total_bsphere;

  if (data_bbox.isValid()) {
    
    // 
    // GET DATA VOLUME SPHERE
    //

    total_bsphere = Sphere( (bboxdeco) ? bboxdeco->getBoundingBox(data_bbox) : data_bbox, getModelViewpoint()->scale );
    if (total_bsphere.radius <= 0.0)
      total_bsphere.radius = 1.0;

  } else {
    total_bsphere = Sphere( Vertex(0,0,0), 1 );
  }
  
  // Now get the matrices.  First we compute the projection matrix.  If we're inheriting,
  // just use the parent.
  
  if (do_projection > EMBED_INHERIT) {
    projMatrix.getData(saveprojection);
    setupProjMatrix(renderContext, total_bsphere);
  } else
    projMatrix = parent->projMatrix;
  
  // Now the model matrix.  Since this depends on both the viewpoint and the model
  // transformations, we don't bother using the parent one, we reconstruct in
  // every subscene.
  
  if (do_projection > EMBED_INHERIT || do_model > EMBED_INHERIT)
    setupModelViewMatrix(renderContext, total_bsphere.center);
  else
    modelMatrix = parent->modelMatrix;
    
  // update subscenes
    
  std::vector<Subscene*>::const_iterator iter;
  for(iter = subscenes.begin(); iter != subscenes.end(); ++iter) 
    (*iter)->update(renderContext);
    
}

void Subscene::render(RenderContext* renderContext, bool opaquePass)
{
  renderContext->subscene = this;
  
  glViewport(pviewport.x, pviewport.y, pviewport.width, pviewport.height);
  glScissor(pviewport.x, pviewport.y, pviewport.width, pviewport.height);
  SAVEGLERROR;
  
  if (background && opaquePass) {
    GLbitfield clearFlags = background->getClearFlags(renderContext);

    // clear
    glDepthMask(GL_TRUE);
    glClear(clearFlags);
  }
  SAVEGLERROR;
  
  // Now render the current scene.  First we set the projection matrix, then the modelview matrix.
  
  double mat[16];
  projMatrix.getData(mat);
  glMatrixMode(GL_PROJECTION);
  glLoadMatrixd(mat);  
  SAVEGLERROR; 
  
  modelMatrix.getData(mat);
  glMatrixMode(GL_MODELVIEW);  
  glLoadMatrixd(mat);
  SAVEGLERROR;
  
  setupLights(renderContext);
  
  if (opaquePass) {
    if (background) {
    //
    // RENDER BACKGROUND
    //

      // DISABLE Z-BUFFER TEST
      glDisable(GL_DEPTH_TEST);

      // DISABLE Z-BUFFER FOR WRITING
      glDepthMask(GL_FALSE);
  
      background->render(renderContext);
      SAVEGLERROR;
    }
 
    //
    // RENDER SOLID SHAPES
    //

    // ENABLE Z-BUFFER TEST 
    glEnable(GL_DEPTH_TEST);

    // ENABLE Z-BUFFER FOR WRITING
    glDepthMask(GL_TRUE);

    // DISABLE BLENDING
    glDisable(GL_BLEND);
    
    //
    // RENDER BBOX DECO
    //

    if (bboxdeco) 
      bboxdeco->render(renderContext);  // This changes the modelview/projection/viewport

    SAVEGLERROR;
  }
  // CLIP PLANES
  renderClipplanes(renderContext);
  
  if (opaquePass) {
    renderUnsorted(renderContext);

// #define NO_BLEND
  } else {
#ifndef NO_BLEND
    //
    // RENDER BLENDED SHAPES
    //
    // render shapes in bounding-box sorted order according to z value
    //

    // DISABLE Z-BUFFER FOR WRITING
    glDepthMask(GL_FALSE);
    
    SAVEGLERROR;
    
    // SETUP BLENDING
    if (renderContext->gl2psActive == GL2PS_NONE) 
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
    else
      gl2psBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

    SAVEGLERROR;
    
    // ENABLE BLENDING
    glEnable(GL_BLEND);

    SAVEGLERROR;

    //
    // GET THE TRANSFORMATION
    //

    Matrix4x4 M(modelMatrix);    
    Matrix4x4 P(projMatrix);
    P = P*M;
    
    Zrow = P.getRow(2);
    Wrow = P.getRow(3);

    renderZsort(renderContext);
#endif    
  }
  /* Reset flag(s) now that scene has been rendered */
  getModelViewpoint()->scaleChanged = false;

  /* Reset clipplanes */
  disableClipplanes(renderContext);
  SAVEGLERROR;
  
  // Render subscenes
    
  std::vector<Subscene*>::const_iterator iter;
  for(iter = subscenes.begin(); iter != subscenes.end(); ++iter) 
    (*iter)->render(renderContext, opaquePass);
  
  if (selectState == msCHANGING) {
    SELECT select;
    select.render(mousePosition);
  }
}

void Subscene::calcDataBBox()
{
  data_bbox.invalidate();
  
  std::vector<Subscene*>::const_iterator subiter;
  bboxChanges = false;
  
  for(subiter = subscenes.begin(); subiter != subscenes.end(); ++subiter) {
    Subscene* subscene = *subiter;
    if (!subscene->getIgnoreExtent()) {
      subscene->calcDataBBox();
      data_bbox += subscene->getBoundingBox();
      bboxChanges |= subscene->bboxChanges;
    }
  }
      
  std::vector<Shape*>::const_iterator iter;
  for(iter = shapes.begin(); iter != shapes.end(); ++iter) {
    Shape* shape = *iter;

    if (!shape->getIgnoreExtent()) {
      data_bbox += shape->getBoundingBox(this);
      bboxChanges |= shape->getBBoxChanges();
    }
  }
  intersectClipplanes(); 
}

void Subscene::intersectClipplanes(void) 
{
  std::vector<ClipPlaneSet*>::iterator iter;
  for (iter = clipPlanes.begin() ; iter != clipPlanes.end() ; ++iter ) {
      ClipPlaneSet* plane = *iter;
      plane->intersectBBox(data_bbox);
      SAVEGLERROR;
  }
}

/* Call this when adding a clipplane that can shrink things */
void Subscene::shrinkBBox(void)
{
  if (parent) parent->shrinkBBox();
  else {
      calcDataBBox();
  }
}
// ---------------------------------------------------------------------------
void Subscene::setIgnoreExtent(int in_ignoreExtent)
{
  ignoreExtent = (bool)in_ignoreExtent;
}

void Subscene::setupViewport(RenderContext* rctx)
{
  Rect2 rect(0,0,0,0);
  if (do_viewport == EMBED_REPLACE) {
    rect.x = rctx->rect.x + viewport.x*rctx->rect.width;
    rect.y = rctx->rect.y + viewport.y*rctx->rect.height;
    rect.width = rctx->rect.width*viewport.width;
    rect.height = rctx->rect.height*viewport.height;
  } else {
    rect.x = parent->pviewport.x + viewport.x*parent->pviewport.width;
    rect.y = parent->pviewport.y + viewport.y*parent->pviewport.height;
    rect.width = parent->pviewport.width*viewport.width;
    rect.height = parent->pviewport.height*viewport.height;
  }
  pviewport = rect;
}

void Subscene::setupProjMatrix(RenderContext* rctx, const Sphere& viewSphere)
{
  if (do_projection == EMBED_REPLACE) 
    projMatrix.setIdentity();

  getUserViewpoint()->setupProjMatrix(rctx, viewSphere);   
}

// The ModelView matrix has components of the user view (the translation at the start)
// and also the model transformations.  The former comes from the userViewpoint,
// the latter from the modelViewpoint, possibly after applying the same from the parents.
// We always reconstruct from scratch rather than trying to use the matrix in place.

void Subscene::setupModelViewMatrix(RenderContext* rctx, Vertex center)
{
  modelMatrix.setIdentity();
  getUserViewpoint()->setupViewer(rctx);
  setupModelMatrix(rctx, center);
}

void Subscene::setupModelMatrix(RenderContext* rctx, Vertex center)
{
  /* The recursive call below will set the active subscene
     modelMatrix, not the inherited one. */
     
  if (do_model < EMBED_REPLACE && parent)
    parent->setupModelMatrix(rctx, center);
    
  if (do_model > EMBED_INHERIT)
    getModelViewpoint()->setupTransformation(rctx, center);
  
}

void Subscene::disableLights(RenderContext* rctx)
{
    
  //
  // disable lights; setup will enable them
  //

  for (int i=0;i<8;i++)
    glDisable(GL_LIGHT0 + i);  
}  

void Subscene::setupLights(RenderContext* rctx) 
{  
  int nlights = 0;
  bool anyviewpoint = false;
  std::vector<Light*>::const_iterator iter;
  
  disableLights(rctx);

  for(iter = lights.begin(); iter != lights.end() ; ++iter ) {

    Light* light = *iter;
    light->id = GL_LIGHT0 + (nlights++);
    if (!light->viewpoint)
      light->setup(rctx);
    else
      anyviewpoint = true;
  }

  SAVEGLERROR;

  if (anyviewpoint) {
    //
    // viewpoint lights
    //

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    for(iter = lights.begin(); iter != lights.end() ; ++iter ) {

      Light* light = *iter;

      if (light->viewpoint)
        light->setup(rctx);
    }
    glPopMatrix();
  }
  SAVEGLERROR;

}

void Subscene::renderUnsorted(RenderContext* renderContext)
{
  std::vector<Shape*>::iterator iter;

  for (iter = unsortedShapes.begin() ; iter != unsortedShapes.end() ; ++iter ) {
    Shape* shape = *iter;
    shape->render(renderContext);
    SAVEGLERROR;
  }
}
    
void Subscene::renderZsort(RenderContext* renderContext)
{  
  std::vector<Shape*>::iterator iter;
  std::multimap<float, ShapeItem*> distanceMap;
  int index = 0;

  for (iter = zsortShapes.begin() ; iter != zsortShapes.end() ; ++iter ) {
    Shape* shape = *iter;
    shape->renderBegin(renderContext);
    for (int j = 0; j < shape->getPrimitiveCount(); j++) {
      ShapeItem* item = new ShapeItem(shape, j);
      float distance = getDistance( shape->getPrimitiveCenter(j) );
      distanceMap.insert( std::pair<const float,ShapeItem*>(-distance, item) );
      index++;
    }
  }

  {
    Shape* prev = NULL;
    std::multimap<float,ShapeItem*>::iterator iter;
    for (iter = distanceMap.begin() ; iter != distanceMap.end() ; ++ iter ) {
      ShapeItem* item = iter->second;
      Shape* shape = item->shape;
      if (shape != prev) {
        if (prev) prev->drawEnd(renderContext);
        shape->drawBegin(renderContext);
        prev = shape;
      }
      shape->drawPrimitive(renderContext, item->itemnum);
      delete item;
    }
    if (prev) prev->drawEnd(renderContext);
  }
}

const AABox& Subscene::getBoundingBox()
{ 
  if (bboxChanges) 
      calcDataBBox();
  return data_bbox; 
}

void Subscene::newEmbedding()
{
  if (parent) {
    if (do_projection == EMBED_REPLACE && !userviewpoint)
      add(new UserViewpoint(*(parent->getUserViewpoint())));
    else if (do_projection == EMBED_MODIFY && !userviewpoint)
      add(new UserViewpoint(0.0, 1.0)); /* should be like an identity */
    
    if (do_model == EMBED_REPLACE && !modelviewpoint)
      add(new ModelViewpoint(*(parent->getModelViewpoint())));
    else if (do_model == EMBED_MODIFY && !modelviewpoint)
      add(new ModelViewpoint(PolarCoord(0.0, 0.0), Vec3(1.0, 1.0, 1.0), 
                             parent->getModelViewpoint()->isInteractive()));
  }
}

void Subscene::setEmbedding(int which, Embedding value)
{
  switch(which) {
    case 0: do_viewport = value; break;
    case 1: do_projection = value; break;
    case 2: do_model = value; break;
    case 3: do_mouseHandlers = value; break;
  }
  newEmbedding();
}

// #include <unistd.h>
Embedding Subscene::getEmbedding(Embedded which)
{
//  Rprintf("getEmbedding %d, subscene %d\n", which, getObjID());
//  usleep(1000000);
  switch(which) {
    case EM_VIEWPORT:      return do_viewport;
    case EM_PROJECTION:    return do_projection;
    case EM_MODEL:         return do_model;
    case EM_MOUSEHANDLERS: return do_mouseHandlers;
    default: error("Bad embedding requested");
  }
}

Subscene* Subscene::getMaster(Embedded which) 
{
  if (getEmbedding(which) == EMBED_INHERIT)
    return getParent()->getMaster(which);
  else
    return this;
}

void Subscene::getUserMatrix(double* dest)
{
  ModelViewpoint* modelviewpoint = getModelViewpoint();
  modelviewpoint->getUserMatrix(dest);
}

void Subscene::setUserMatrix(double* src)
{
  ModelViewpoint* modelviewpoint = getModelViewpoint();
  modelviewpoint->setUserMatrix(src);
}

void Subscene::getUserProjection(double* dest)
{
  UserViewpoint* userviewpoint = getUserViewpoint();
  userviewpoint->getUserProjection(dest);
}

void Subscene::setUserProjection(double* src)
{
  UserViewpoint* userviewpoint = getUserViewpoint();
  userviewpoint->setUserProjection(src);
}

void Subscene::getScale(double* dest)
{
  ModelViewpoint* modelviewpoint = getModelViewpoint();
  modelviewpoint->getScale(dest);
}

void Subscene::setScale(double* src)
{
  ModelViewpoint* modelviewpoint = getModelViewpoint();
  modelviewpoint->setScale(src);
}

void Subscene::getPosition(double* dest)
{
  ModelViewpoint* modelviewpoint = getModelViewpoint();
  modelviewpoint->getPosition(dest);
}

void Subscene::setPosition(double* src)
{
  ModelViewpoint* modelviewpoint = getModelViewpoint();
  modelviewpoint->setPosition(src);
}

void Subscene::setViewport(double x, double y, double width, double height)
{
  viewport.x = x;
  viewport.y = y;
  viewport.width = width;
  viewport.height = height;
}

void Subscene::clearMouseListeners()
{
  mouseListeners.clear();
}

void Subscene::addMouseListener(Subscene* sub)
{
  mouseListeners.push_back(sub);
}

void Subscene::deleteMouseListener(Subscene* sub)
{
  for (int i=0; i < mouseListeners.size(); i++) {
    if (sub == mouseListeners[i]) {
      mouseListeners.erase(mouseListeners.begin() + i);
      return;
    }
  }
}

void Subscene::getMouseListeners(unsigned int max, int* ids)
{
  max = max > mouseListeners.size() ? mouseListeners.size() : max;  
  for (unsigned int i = 0; i < max; i++)
    ids[i] = mouseListeners[i]->getObjID();
}

float Subscene::getDistance(const Vertex& v) const
{
  Vertex4 vec = Vertex4(v, 1.0f);

  return (Zrow*vec) / (Wrow*vec);
}

viewControlPtr Subscene::getButtonBeginFunc(int which) {
  return getMaster(EM_MOUSEHANDLERS)->ButtonBeginFunc[which];
}

void Subscene::buttonBegin(int which, int mouseX, int mouseY)
{
  // Rprintf("Subscene %d::buttonBegin %d\n",  getObjID(), which);
  (this->*getButtonBeginFunc(which))(mouseX, mouseY);
}

viewControlPtr Subscene::getButtonUpdateFunc(int which) 
{  
  return getMaster(EM_MOUSEHANDLERS)->ButtonUpdateFunc[which];
}

void Subscene::buttonUpdate(int which, int mouseX, int mouseY)
{
  (this->*getButtonUpdateFunc(which))(mouseX, mouseY);
}

viewControlEndPtr Subscene::getButtonEndFunc(int which)
{
  return getMaster(EM_MOUSEHANDLERS)->ButtonEndFunc[which];  
}

void Subscene::buttonEnd(int which)
{
  (this->*getButtonEndFunc(which))();
}

void Subscene::setDefaultMouseMode()
{
  setMouseMode(1, mmPOLAR);
  setMouseMode(2, mmFOV);
  setMouseMode(3, mmZOOM);
  setWheelMode(wmNONE);
}

void Subscene::setMouseMode(int button, MouseModeID mode)
{
  if (getEmbedding(EM_MOUSEHANDLERS) == EMBED_INHERIT)
    getParent()->setMouseMode(button, mode);
  else {
    int index = button-1;
    mouseMode[index] = mode;
    switch (mode) {
    case mmNONE:
      ButtonBeginFunc[index] = &Subscene::noneBegin;
      ButtonUpdateFunc[index] = &Subscene::noneUpdate;
      ButtonEndFunc[index] = &Subscene::noneEnd;
      break;
    case mmTRACKBALL:
      ButtonBeginFunc[index] = &Subscene::trackballBegin;
      ButtonUpdateFunc[index] = &Subscene::trackballUpdate;
      ButtonEndFunc[index] = &Subscene::trackballEnd;
      break;
    case mmXAXIS:
    case mmYAXIS:
    case mmZAXIS:
      ButtonBeginFunc[index] = &Subscene::oneAxisBegin;
      ButtonUpdateFunc[index] = &Subscene::oneAxisUpdate;
      ButtonEndFunc[index] = &Subscene::trackballEnd; // No need for separate function
      if (mode == mmXAXIS)      axis[index] = Vertex(1,0,0);
      else if (mode == mmYAXIS) axis[index] = Vertex(0,1,0);
      else                      axis[index] = Vertex(0,0,1);
      break;	    	
    case mmPOLAR:
      ButtonBeginFunc[index] = &Subscene::polarBegin;
      ButtonUpdateFunc[index] = &Subscene::polarUpdate;
      ButtonEndFunc[index] = &Subscene::polarEnd;
      break;
    case mmSELECTING:
      ButtonBeginFunc[index] = &Subscene::mouseSelectionBegin;
      ButtonUpdateFunc[index] = &Subscene::mouseSelectionUpdate;
      ButtonEndFunc[index] = &Subscene::mouseSelectionEnd;
      break;
    case mmZOOM:
      ButtonBeginFunc[index] = &Subscene::adjustZoomBegin;
      ButtonUpdateFunc[index] = &Subscene::adjustZoomUpdate;
      ButtonEndFunc[index] = &Subscene::adjustZoomEnd;
      break;
    case mmFOV:
      ButtonBeginFunc[index] = &Subscene::adjustFOVBegin;
      ButtonUpdateFunc[index] = &Subscene::adjustFOVUpdate;
      ButtonEndFunc[index] = &Subscene::adjustFOVEnd;
      break;
    case mmUSER:
      ButtonBeginFunc[index] = &Subscene::userBegin;
      ButtonUpdateFunc[index] = &Subscene::userUpdate;
      ButtonEndFunc[index] = &Subscene::userEnd;
      break;	    	
    }
  }
}

void Subscene::setMouseCallbacks(int button, userControlPtr begin, userControlPtr update, 
                                 userControlEndPtr end, userCleanupPtr cleanup, void** user)
{
  if (getEmbedding(EM_MOUSEHANDLERS) == EMBED_INHERIT)
    getParent()->setMouseCallbacks(button, begin, update, end, cleanup, user);  
  else {
    int ind = button - 1;
    if (cleanupCallback[ind])
      (*cleanupCallback[ind])(userData + 3*ind);
    beginCallback[ind] = begin;
    updateCallback[ind] = update;
    endCallback[ind] = end;
    cleanupCallback[ind] = cleanup;
    userData[3*ind + 0] = *(user++);
    userData[3*ind + 1] = *(user++);
    userData[3*ind + 2] = *user;
    setMouseMode(button, mmUSER);
  }
}

void Subscene::getMouseCallbacks(int button, userControlPtr *begin, userControlPtr *update, 
                                 userControlEndPtr *end, userCleanupPtr *cleanup, void** user)
{
  if (getEmbedding(EM_MOUSEHANDLERS) == EMBED_INHERIT)
    getParent()->getMouseCallbacks(button, begin, update, end, cleanup, user); 
  else {
    int ind = button - 1;
    *begin = beginCallback[ind];
    *update = updateCallback[ind];
    *end = endCallback[ind];
    *cleanup = cleanupCallback[ind];
    *(user++) = userData[3*ind + 0];
    *(user++) = userData[3*ind + 1];
    *(user++) = userData[3*ind + 2];
  }
} 

MouseModeID Subscene::getMouseMode(int button)
{
  return getMaster(EM_MOUSEHANDLERS)->mouseMode[button-1];
}

void Subscene::setWheelMode(WheelModeID mode)
{
  if (getEmbedding(EM_MOUSEHANDLERS) == EMBED_INHERIT)
    getParent()->setWheelMode(mode); 
  else {
    wheelMode = mode;
    switch (mode) {
    case wmNONE:
      WheelRotateFunc = &Subscene::wheelRotateNone;
      break;
    case wmPULL:
      WheelRotateFunc = &Subscene::wheelRotatePull;
      break;
    case wmPUSH:
      WheelRotateFunc = &Subscene::wheelRotatePush;
      break;
    case wmUSER:
      WheelRotateFunc = &Subscene::userWheel;
      break;
    }
  }
}

WheelModeID Subscene::getWheelMode()
{
  return getMaster(EM_MOUSEHANDLERS)->wheelMode;
}

void Subscene::setWheelCallback(userWheelPtr wheel, void* user)
{
  if (getEmbedding(EM_MOUSEHANDLERS) == EMBED_INHERIT)
    getParent()->setWheelCallback(wheel, user);
  else {
    wheelCallback = wheel;
    wheelData = user;
    setWheelMode(wmUSER); 
  }
}

void Subscene::getWheelCallback(userWheelPtr *wheel, void** user)
{
  if (getEmbedding(EM_MOUSEHANDLERS) == EMBED_INHERIT)
    getParent()->getWheelCallback(wheel, user);   
  *wheel = wheelCallback;
  *user = wheelData;
}


//
// FUNCTION
//   screenToPolar
//
// DESCRIPTION
//   screen space is the same as in OpenGL, starting 0,0 at left/bottom(!) of viewport
//

static PolarCoord screenToPolar(int width, int height, int mouseX, int mouseY) {
  
  float cubelen, cx,cy,dx,dy,r;
  
  cubelen = (float) getMin(width,height);
  r   = cubelen * 0.5f;
  
  cx  = ((float)width)  * 0.5f;
  cy  = ((float)height) * 0.5f;
  dx  = ((float)mouseX) - cx;
  dy  = ((float)mouseY) - cy;
  
  //
  // dx,dy = distance to center in pixels
  //
  
  dx = clamp(dx, -r,r);
  dy = clamp(dy, -r,r);
  
  //
  // sin theta = dx / r
  // sin phi   = dy / r
  //
  // phi   = arc sin ( sin theta )
  // theta = arc sin ( sin phi   )
  //
  
  return PolarCoord(
    
    math::rad2deg( math::asin( dx/r ) ),
    math::rad2deg( math::asin( dy/r ) )
    
  );
  
}

static Vertex screenToVector(int width, int height, int mouseX, int mouseY) {
  
  float radius = (float) getMax(width, height) * 0.5f;
  
  float cx = ((float)width) * 0.5f;
  float cy = ((float)height) * 0.5f;
  float x  = (((float)mouseX) - cx)/radius;
  float y  = (((float)mouseY) - cy)/radius;
  
  // Make unit vector
  
  float len = sqrt(x*x + y*y);
  if (len > 1.0e-6) {
    x = x/len;
    y = y/len;
  }
  // Find length to first edge
  
  float maxlen = math::sqrt(2.0f);
  
  // zero length is vertical, max length is horizontal
  float angle = (maxlen - len)/maxlen*math::pi<float>()/2.0f;
  
  float z = math::sin(angle);
  
  // renorm to unit length
  
  len = math::sqrt(1.0f - z*z);
  x = x*len;
  y = y*len;
  
  return Vertex(x, y, z);
}

void Subscene::trackballBegin(int mouseX, int mouseY)
{
  rotBase = screenToVector(pviewport.width,pviewport.height,mouseX,mouseY);
}

void Subscene::trackballUpdate(int mouseX, int mouseY)
{
  rotCurrent = screenToVector(pviewport.width,pviewport.height,mouseX,mouseY);

  for (unsigned int i = 0; i < mouseListeners.size(); i++) {
    Subscene* sub = mouseListeners[i];
    if (sub) {
      ModelViewpoint* modelviewpoint = sub->getModelViewpoint();
      modelviewpoint->updateMouseMatrix(rotBase,rotCurrent);
    }
  }
}

void Subscene::trackballEnd()
{
  for (unsigned int i = 0; i < mouseListeners.size(); i++) {
    Subscene* sub = mouseListeners[i];
    if (sub) {   
      ModelViewpoint* modelviewpoint = sub->getModelViewpoint();
      modelviewpoint->mergeMouseMatrix();
    }
  }
}

void Subscene::oneAxisBegin(int mouseX, int mouseY)
{
  rotBase = screenToVector(pviewport.width,pviewport.height,mouseX,pviewport.height/2);
}

void Subscene::oneAxisUpdate(int mouseX, int mouseY)
{
  rotCurrent = screenToVector(pviewport.width,pviewport.height,mouseX,pviewport.height/2);
  
  for (unsigned int i = 0; i < mouseListeners.size(); i++) {
    Subscene* sub = mouseListeners[i];
    if (sub) {
      ModelViewpoint* modelviewpoint = sub->getModelViewpoint();
      modelviewpoint->mouseOneAxis(rotBase,rotCurrent,axis[drag-1]);
    }
    
  }
}

void Subscene::polarBegin(int mouseX, int mouseY)
{
  ModelViewpoint* modelviewpoint = getModelViewpoint();
  
  camBase = modelviewpoint->getPosition();
  
  dragBase = screenToPolar(pviewport.width,pviewport.height,mouseX,mouseY);
  
}

void Subscene::polarUpdate(int mouseX, int mouseY)
{
  dragCurrent = screenToPolar(pviewport.width,pviewport.height,mouseX,mouseY);
  
  PolarCoord newpos = camBase - ( dragCurrent - dragBase );
  
  newpos.phi = clamp( newpos.phi, -90.0f, 90.0f );
  for (unsigned int i = 0; i < mouseListeners.size(); i++) {
    Subscene* sub = mouseListeners[i];
    if (sub) {   
      ModelViewpoint* modelviewpoint = sub->getModelViewpoint();
      modelviewpoint->setPosition( newpos );
    }
  }
}

void Subscene::polarEnd()
{
  
  //    Viewpoint* viewpoint = scene->getViewpoint();
  //    viewpoint->mergeMouseMatrix();
  
}

void Subscene::adjustFOVBegin(int mouseX, int mouseY)
{
  fovBaseY = mouseY;
}


void Subscene::adjustFOVUpdate(int mouseX, int mouseY)
{
  int dy = mouseY - fovBaseY;
  
  float py = -((float)dy/(float)pviewport.height) * 180.0f;
  
  for (unsigned int i = 0; i < mouseListeners.size(); i++) {
    Subscene* sub = mouseListeners[i];
    if (sub) {
      UserViewpoint* userviewpoint = sub->getUserViewpoint();
      userviewpoint->setFOV( userviewpoint->getFOV() + py );
    }
  }
  
  fovBaseY = mouseY;
}


void Subscene::adjustFOVEnd()
{
}

void Subscene::wheelRotatePull(int dir)
{
  for (unsigned int i = 0; i < mouseListeners.size(); i++) {
    Subscene* sub = mouseListeners[i];
    if (sub) {
      UserViewpoint* userviewpoint = sub->getUserViewpoint();
      float zoom = userviewpoint->getZoom();
      
#define ZOOM_STEP  1.05f 
#define ZOOM_PIXELLOGSTEP 0.02f
#define ZOOM_MIN  0.0001f
#define ZOOM_MAX  10000.0f      
      switch(dir)
      {
      case GUI_WheelForward:
        zoom *= ZOOM_STEP;
        break;
      case GUI_WheelBackward:
        zoom /= ZOOM_STEP;
        break;
      }
      
      zoom = clamp( zoom , ZOOM_MIN, ZOOM_MAX);
      userviewpoint->setZoom(zoom);
    }
  }
}

void Subscene::wheelRotatePush(int dir)
{
  switch (dir)
  {
  case GUI_WheelForward:
    wheelRotatePull(GUI_WheelBackward);
    break;
  case GUI_WheelBackward:
    wheelRotatePull(GUI_WheelForward);
    break;
  }
}

void Subscene::wheelRotate(int dir)
{
  (this->*WheelRotateFunc)(dir);
}

void Subscene::userBegin(int mouseX, int mouseY)
{
  int ind = drag - 1;
  Subscene* master = getMaster(EM_MOUSEHANDLERS);
  beginCallback[ind] = master->beginCallback[ind];
  void* userData = master->userData[3*ind+0];
  // Rprintf("userBegin in %d with ind=%d\n", getObjID(), ind);
  activeButton = drag;
  if (beginCallback[ind]) {
    busy = true;
    (*beginCallback[ind])(userData, mouseX, pviewport.height-mouseY);
    busy = false;
  }
}


void Subscene::userUpdate(int mouseX, int mouseY)
{
  int ind = activeButton - 1;
  Subscene* master = getMaster(EM_MOUSEHANDLERS);
  updateCallback[ind] = master->updateCallback[ind];
  void* userData = master->userData[3*ind+1];
  if (!busy && updateCallback[ind]) {
    busy = true;
    (*updateCallback[ind])(userData, mouseX, pviewport.height-mouseY);
    busy = false;
  }
}

void Subscene::userEnd()
{
  int ind = activeButton - 1;
  Subscene* master = getMaster(EM_MOUSEHANDLERS);
  endCallback[ind] = master->endCallback[ind];
  void* userData = master->userData[3*ind+2];
  if (endCallback[ind])
    (*endCallback[ind])(userData);
}

void Subscene::userWheel(int dir)
{
  wheelCallback = getMaster(EM_MOUSEHANDLERS)->wheelCallback;
  if (wheelCallback)
    (*wheelCallback)(wheelData, dir);
}

void Subscene::adjustZoomBegin(int mouseX, int mouseY)
{
  zoomBaseY = mouseY;
}


void Subscene::adjustZoomUpdate(int mouseX, int mouseY)
{
  int dy = mouseY - zoomBaseY;
  // Rprintf("adjustZoomUpdate by %d for %d\n", dy, getObjID());
  for (unsigned int i = 0; i < mouseListeners.size(); i++) {
    // Rprintf("adjustZoomUpdate: mouseListeners[%d]=%d\n", i, mouseListeners[i]);
    Subscene* sub = mouseListeners[i];
    if (sub) {
//      Rprintf("found it\n");
      UserViewpoint* userviewpoint = sub->getUserViewpoint();
      
      float zoom = clamp ( userviewpoint->getZoom() * exp(dy*ZOOM_PIXELLOGSTEP), ZOOM_MIN, ZOOM_MAX);
      // Rprintf("zoom = %f for subscene %d\n", zoom, sub->getObjID());
      userviewpoint->setZoom(zoom);
    }
  }
  
  zoomBaseY = mouseY;
}


void Subscene::adjustZoomEnd()
{
}

double* Subscene::getMousePosition()
{
  return mousePosition;
}

MouseSelectionID Subscene::getSelectState() 
{
  return selectState;
}

void Subscene::setSelectState(MouseSelectionID state)
{
  selectState = state;
}

void Subscene::mouseSelectionBegin(int mouseX,int mouseY)
{
  if (selectState == msABORT) return;
  
  mousePosition[0] = (float)mouseX/(float)pviewport.width;
  mousePosition[1] = (float)mouseY/(float)pviewport.height;
  mousePosition[2] = mousePosition[0];
  mousePosition[3] = mousePosition[1];
  selectState = msCHANGING;
}

void Subscene::mouseSelectionUpdate(int mouseX,int mouseY)
{
  mousePosition[2] = (float)mouseX/(float)pviewport.width;
  mousePosition[3] = (float)mouseY/(float)pviewport.height;
}

void Subscene::mouseSelectionEnd()
{
  if (selectState == msABORT) return;
  
  selectState = msDONE;
}
